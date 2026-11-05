// Funciones de vídeo del kernel (Vd*) y modo de vídeo. Conectan al juego con el procesador de
// comandos de la GPU (gpu/command_processor). Convención: argumentos en r3..r10, resultado en r3.
// Los formatos de las estructuras y los valores devueltos siguen a Xenia.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "imports.h"
#include "../gpu/command_processor.h"
#include "../memory.h"

namespace
{
    constexpr uint32_t kDisplayWidth = 1280, kDisplayHeight = 720;

    bool Trace() { return std::getenv("PROSTREET_TRACE") != nullptr; }

    void WriteFloat(uint32_t address, float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        memory::Write32(address, bits);
    }

    // X_VIDEO_MODE (0x30 bytes): 720p panorámico a 60 Hz, NTSC.
    void WriteVideoMode(uint32_t mode)
    {
        std::memset(memory::Translate(mode), 0, 0x30);
        memory::Write32(mode + 0x00, kDisplayWidth);
        memory::Write32(mode + 0x04, kDisplayHeight);
        memory::Write32(mode + 0x08, 0);    // entrelazado
        memory::Write32(mode + 0x0C, 1);    // panorámico
        memory::Write32(mode + 0x10, 1);    // alta definición
        WriteFloat(mode + 0x14, 60.0f);     // frecuencia
        memory::Write32(mode + 0x18, 1);    // estándar de vídeo: NTSC-U
        memory::Write32(mode + 0x1C, 0x4A);
        memory::Write32(mode + 0x20, 0x01);
    }
}

// void XGetVideoMode(X_VIDEO_MODE* mode)
PPC_FUNC(__imp__XGetVideoMode)
{
    (void)base;
    WriteVideoMode(ctx.r3.u32);
}

// void VdQueryVideoMode(X_VIDEO_MODE* mode)
PPC_FUNC(__imp__VdQueryVideoMode)
{
    (void)base;
    WriteVideoMode(ctx.r3.u32);
}

// uint32_t VdQueryVideoFlags(): 1 = panorámico, 2 = ancho >= 1024, 4 = ancho >= 1920.
PPC_FUNC(__imp__VdQueryVideoFlags)
{
    (void)base;
    ctx.r3.u64 = 1 | 2;
}

// void VdGetCurrentDisplayGamma(uint32_t* type, float* power)
PPC_FUNC(__imp__VdGetCurrentDisplayGamma)
{
    (void)base;
    memory::Write32(ctx.r3.u32, 2);
    WriteFloat(ctx.r4.u32, 2.22222233f);
}

// uint32_t MmGetPhysicalAddress(void* address): la GPU trabaja con direcciones físicas.
PPC_FUNC(__imp__MmGetPhysicalAddress)
{
    (void)base;
    ctx.r3.u64 = gpu::GuestToPhysical(ctx.r3.u32);
}

// uint32_t VdInitializeEngines(uint32_t unknown, void* callback, void* argument, ...): 1 = éxito.
PPC_FUNC(__imp__VdInitializeEngines)
{
    (void)base;
    ctx.r3.u64 = 1;
}

// void VdShutdownEngines()
PPC_FUNC(__imp__VdShutdownEngines)
{
    (void)base;
}

// BOOL VdIsHSIOTrainingSucceeded(): el enlace con la GPU se calibró bien. Si devuelve 0, D3D
// avisa "GPU initialization (HSIO training) has failed so no graphics will render".
PPC_FUNC(__imp__VdIsHSIOTrainingSucceeded)
{
    (void)base;
    ctx.r3.u64 = 1;
}

// void VdInitializeRingBuffer(uint32_t physicalAddress, uint32_t sizeLog2)
PPC_FUNC(__imp__VdInitializeRingBuffer)
{
    (void)base;
    gpu::InitializeRingBuffer(ctx.r3.u32, ctx.r4.u32);
}

// void VdEnableRingBufferRPtrWriteBack(uint32_t physicalAddress, uint32_t blockLog2)
PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack)
{
    (void)base;
    gpu::EnableReadPointerWriteBack(ctx.r3.u32, ctx.r4.u32);
}

// void VdSetGraphicsInterruptCallback(void* callback, void* userData)
PPC_FUNC(__imp__VdSetGraphicsInterruptCallback)
{
    (void)base;
    if (Trace())
        std::fprintf(stderr, "[gpu] callback de interrupción 0x%08X (datos 0x%08X)\n", ctx.r3.u32, ctx.r4.u32);
    gpu::SetInterruptCallback(ctx.r3.u32, ctx.r4.u32);
}

// void VdGetSystemCommandBuffer(uint32_t* buffer, uint32_t* value)
PPC_FUNC(__imp__VdGetSystemCommandBuffer)
{
    (void)base;
    memory::Write32(ctx.r3.u32, 0xBEEF0000);
    memory::Write32(ctx.r4.u32, 0xBEEF0001);
}

// uint32_t VdInitializeScalerCommandBuffer(10 argumentos..., uint32_t* destination, uint32_t count)
// 12 argumentos: el destino y el número de palabras son el 11.º y el 12.º (en la pila). El juego
// rellena antes el destino con NOP (RtlFillMemoryUlong, 800 bytes) y pasa 200. Se deja con NOP:
// el juego solo comprueba el resultado.
PPC_FUNC(__imp__VdInitializeScalerCommandBuffer)
{
    (void)base;
    const uint32_t destination = StackArgument(ctx, 2), count = StackArgument(ctx, 3);
    for (uint32_t i = 0; i < count; i++)
        memory::Write32(destination + i * 4, 0x80000000);
    ctx.r3.u64 = count;
}

// uint32_t VdRetrainEDRAM(...), VdRetrainEDRAMWorker(...): sin EDRAM real que calibrar.
PPC_FUNC(__imp__VdRetrainEDRAM)
{
    (void)base;
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdRetrainEDRAMWorker)
{
    (void)base;
    ctx.r3.u64 = 0;
}

// void VdSwap(uint32_t* buffer, void* fetch, void* unknown2, void* unknown3, void* unknown4,
//             uint32_t* frontBuffer, uint32_t* format, uint32_t* colorSpace, uint32_t* width, uint32_t* height)
// `buffer` apunta a 64 palabras reservadas en el ring: se rellenan con un paquete propio que el
// procesador de comandos reconoce al presentar el fotograma, y NOP.
PPC_FUNC(__imp__VdSwap)
{
    (void)base;
    const uint32_t buffer = ctx.r3.u32;
    const uint32_t frontBufferPtr = ctx.r8.u32, widthPtr = StackArgument(ctx, 0), heightPtr = StackArgument(ctx, 1);
    memory::Write32(buffer + 0, (3u << 30) | ((63u - 1) << 16) | (gpu::kPacketSwap << 8));
    memory::Write32(buffer + 4, gpu::kSwapMagic);
    memory::Write32(buffer + 8, frontBufferPtr ? memory::Read32(frontBufferPtr) : 0);
    memory::Write32(buffer + 12, widthPtr ? memory::Read32(widthPtr) : 0);
    memory::Write32(buffer + 16, heightPtr ? memory::Read32(heightPtr) : 0);
    for (uint32_t i = 5; i < 64; i++)
        memory::Write32(buffer + i * 4, 0x80000000); // NOP (paquete de tipo 2)
}

// void VdGetCurrentDisplayInformation(X_DISPLAY_INFO* info)
// Solo se rellenan los campos de posición segura (ancho y alto del front buffer, al principio).
PPC_FUNC(__imp__VdGetCurrentDisplayInformation)
{
    (void)base;
    const uint32_t info = ctx.r3.u32;
    std::memset(memory::Translate(info), 0, 0x58);
    memory::Write16(info + 0, kDisplayWidth);
    memory::Write16(info + 2, kDisplayHeight);
}

// Llamadas de configuración de la pantalla y del procesador de comandos del sistema que no
// cambian nada aquí: el modo de vídeo es fijo (720p) y el front buffer llega por VdSwap.

// void VdSetSystemCommandBufferGpuIdentifierAddress(void* address)
PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress)
{
    (void)base;
}

// uint32_t VdCallGraphicsNotificationRoutines(uint32_t unknown, ...)
PPC_FUNC(__imp__VdCallGraphicsNotificationRoutines)
{
    (void)base;
    ctx.r3.u64 = 0;
}

// uint32_t VdSetDisplayMode(uint32_t mode)
PPC_FUNC(__imp__VdSetDisplayMode)
{
    (void)base;
    ctx.r3.u64 = 0;
}

// void VdPersistDisplay(uint32_t unknown, uint32_t* physicalAddress): sin imagen que conservar
// entre títulos (0 = nada que liberar después con MmFreePhysicalMemory).
PPC_FUNC(__imp__VdPersistDisplay)
{
    (void)base;
    if (ctx.r4.u32)
        memory::Write32(ctx.r4.u32, 0);
}
