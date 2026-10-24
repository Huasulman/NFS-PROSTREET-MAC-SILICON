// XMA: el decodificador de audio por hardware. Cada contexto son 64 bytes en memoria física
// (XMA_CONTEXT_DATA, 16 palabras big-endian con campos de bits, según Xenia) que describen los
// búferes de entrada (paquetes XMA) y el de salida (PCM) de una voz.
//
// De momento solo se implementan los contextos: todavía no se decodifica nada, así que el
// búfer de salida no avanza (las voces XMA suenan en silencio).

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "imports.h"
#include "../memory.h"

namespace
{
    constexpr uint32_t kRegisterBase = 0x7FEA0000;            // registros del decodificador (MMIO)
    constexpr uint32_t kRegContextArrayAddress = 0x1800;
    constexpr uint32_t kContextSize = 64;
    constexpr uint32_t kContextCount = 320; // como el hardware

    std::mutex gLock;
    uint32_t gContexts = 0; // bloque de kContextCount contextos en memoria física
    std::vector<bool> gUsed(kContextCount);

    // Un campo de bits de la palabra `word` del contexto (bit 0 = el menos significativo).
    struct Field
    {
        uint32_t word, shift, bits;
    };

    // Palabra 0
    constexpr Field kInput0PacketCount{0, 0, 12};
    constexpr Field kLoopCount{0, 12, 8};
    constexpr Field kInput0Valid{0, 20, 1};
    constexpr Field kInput1Valid{0, 21, 1};
    constexpr Field kOutputBlockCount{0, 22, 5};
    constexpr Field kOutputWriteOffset{0, 27, 5};
    // Palabra 1
    constexpr Field kInput1PacketCount{1, 0, 12};
    constexpr Field kLoopSubframeEnd{1, 14, 3};
    constexpr Field kLoopSubframeSkip{1, 17, 3};
    constexpr Field kSubframeDecodeCount{1, 20, 4};
    constexpr Field kSampleRate{1, 27, 2};
    constexpr Field kStereo{1, 29, 1};
    constexpr Field kOutputValid{1, 31, 1};
    // Palabras 2 a 9
    constexpr Field kInputReadOffset{2, 0, 26};
    constexpr Field kLoopStart{3, 0, 26};
    constexpr Field kLoopEnd{4, 0, 26};
    constexpr Field kInput0Pointer{5, 0, 32};
    constexpr Field kInput1Pointer{6, 0, 32};
    constexpr Field kOutputPointer{7, 0, 32};
    constexpr Field kWorkPointer{8, 0, 32};
    constexpr Field kOutputReadOffset{9, 0, 5};

    uint32_t Mask(const Field& field)
    {
        return field.bits == 32 ? 0xFFFFFFFFu : ((1u << field.bits) - 1) << field.shift;
    }

    uint32_t Get(uint32_t context, const Field& field)
    {
        return (memory::Read32(context + field.word * 4) & Mask(field)) >> field.shift;
    }

    void Set(uint32_t context, const Field& field, uint32_t value)
    {
        const uint32_t address = context + field.word * 4;
        const uint32_t mask = Mask(field);
        memory::Write32(address, (memory::Read32(address) & ~mask) | ((value << field.shift) & mask));
    }
}

// Tabla de contextos del decodificador: XAudio lee su dirección física del registro
// CONTEXT_ARRAY_ADDRESS (0x7FEA1800, en little-endian) y trabaja con ella directamente; sin ella
// cree que no hay decodificador XMA y descarta esas voces (música, sonidos de los menús).
void XmaInitialize()
{
    std::lock_guard lock(gLock);
    // Es la primera reserva física: sin este hueco quedaría en la dirección física 0, que para
    // XAudio significa "no hay tabla".
    memory::Allocate(memory::kPhysicalBase, memory::kPhysicalEnd, 0x10000, 0x1000);
    gContexts = memory::Allocate(memory::kPhysicalBase, memory::kPhysicalEnd, kContextSize * kContextCount, 0x1000);
    memory::Write32(kRegisterBase + kRegContextArrayAddress, __builtin_bswap32(gContexts & 0x1FFFFFFF));
}

// uint32_t XMACreateContext(uint32_t* context)
PPC_FUNC(__imp__XMACreateContext)
{
    (void)base;
    std::lock_guard lock(gLock);
    for (uint32_t index = 0; index < kContextCount; index++)
    {
        if (gUsed[index])
            continue;
        gUsed[index] = true;
        const uint32_t context = gContexts + index * kContextSize;
        for (uint32_t offset = 0; offset < kContextSize; offset += 4)
            memory::Write32(context + offset, 0);
        memory::Write32(ctx.r3.u32, context);
        ctx.r3.u64 = 0;
        return;
    }
    std::fprintf(stderr, "[xma] XMACreateContext: no quedan contextos\n");
    ctx.r3.u64 = 0xC0000017; // STATUS_NO_MEMORY
}

// void XMAReleaseContext(XMA_CONTEXT_DATA* context)
PPC_FUNC(__imp__XMAReleaseContext)
{
    (void)base;
    std::lock_guard lock(gLock);
    const uint32_t index = (ctx.r3.u32 - gContexts) / kContextSize;
    if (gContexts && index < kContextCount)
        gUsed[index] = false;
    ctx.r3.u64 = 0;
}

// uint32_t XMAInitializeContext(XMA_CONTEXT_DATA* context, XMA_CONTEXT_INIT* init)
// XMA_CONTEXT_INIT: input0 ptr, input0 packets, input1 ptr, input1 packets, read offset,
// output ptr, output blocks, work buffer, subframes, canales, frecuencia y datos de bucle.
PPC_FUNC(__imp__XMAInitializeContext)
{
    (void)base;
    const uint32_t context = ctx.r3.u32, init = ctx.r4.u32;
    for (uint32_t offset = 0; offset < kContextSize; offset += 4)
        memory::Write32(context + offset, 0);
    auto field = [&](uint32_t index) { return memory::Read32(init + index * 4); };
    Set(context, kInput0Pointer, field(0));
    Set(context, kInput0PacketCount, field(1));
    Set(context, kInput1Pointer, field(2));
    Set(context, kInput1PacketCount, field(3));
    Set(context, kInputReadOffset, field(4));
    Set(context, kOutputPointer, field(5));
    Set(context, kOutputBlockCount, field(6));
    Set(context, kWorkPointer, field(7));
    Set(context, kSubframeDecodeCount, field(8));
    Set(context, kStereo, field(9) >= 1);
    Set(context, kSampleRate, field(10));
    Set(context, kLoopStart, field(11));
    Set(context, kLoopEnd, field(12));
    const uint32_t loop = field(13); // loop count, subframe end, subframe skip (bytes)
    Set(context, kLoopCount, loop >> 24);
    Set(context, kLoopSubframeEnd, (loop >> 16) & 0xFF);
    Set(context, kLoopSubframeSkip, (loop >> 8) & 0xFF);
    if (std::getenv("PROSTREET_TRACE"))
        std::fprintf(stderr, "[xma] init 0x%08X: entrada 0x%08X (%u paq.) / 0x%08X (%u paq.), salida 0x%08X (%u bloques), "
            "frecuencia %u, canales %u, bucle %u-%u x%u\n", context, field(0), field(1), field(2), field(3), field(5),
            field(6), field(10), field(9), field(11), field(12), loop >> 24);
    ctx.r3.u64 = 0;
}

// El hardware real se arranca y se para con estas dos; sin decodificador no hacen nada.
PPC_FUNC(__imp__XMAEnableContext)
{
    (void)base;
    if (std::getenv("PROSTREET_TRACE"))
    {
        std::fprintf(stderr, "[xma] activar 0x%08X:", ctx.r3.u32);
        for (uint32_t i = 0; i < 16; i++)
            std::fprintf(stderr, " %08X", memory::Read32(ctx.r3.u32 + i * 4));
        std::fprintf(stderr, "\n");
    }
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XMADisableContext)
{
    (void)base;
    if (std::getenv("PROSTREET_TRACE"))
        std::fprintf(stderr, "[xma] desactivar 0x%08X\n", ctx.r3.u32);
    ctx.r3.u64 = 0;
}

// Búferes de entrada: (context, buffer, packetCount) y sus indicadores de validez.
PPC_FUNC(__imp__XMASetInputBuffer0)
{
    (void)base;
    if (std::getenv("PROSTREET_TRACE"))
        std::fprintf(stderr, "[xma] entrada0 0x%08X: 0x%08X %u paq.\n", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    Set(ctx.r3.u32, kInput0Pointer, ctx.r4.u32);
    Set(ctx.r3.u32, kInput0PacketCount, ctx.r5.u32);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XMASetInputBuffer1)
{
    (void)base;
    Set(ctx.r3.u32, kInput1Pointer, ctx.r4.u32);
    Set(ctx.r3.u32, kInput1PacketCount, ctx.r5.u32);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XMAIsInputBuffer0Valid)
{
    (void)base;
    ctx.r3.u64 = Get(ctx.r3.u32, kInput0Valid);
}

PPC_FUNC(__imp__XMAIsInputBuffer1Valid)
{
    (void)base;
    ctx.r3.u64 = Get(ctx.r3.u32, kInput1Valid);
}

PPC_FUNC(__imp__XMASetInputBuffer0Valid)
{
    (void)base;
    Set(ctx.r3.u32, kInput0Valid, 1);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XMASetInputBuffer1Valid)
{
    (void)base;
    Set(ctx.r3.u32, kInput1Valid, 1);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XMASetInputBufferReadOffset)
{
    (void)base;
    Set(ctx.r3.u32, kInputReadOffset, ctx.r4.u32);
    ctx.r3.u64 = 0;
}

// Búfer de salida (PCM, en bloques de 256 bytes).
PPC_FUNC(__imp__XMAGetOutputBufferWriteOffset)
{
    (void)base;
    ctx.r3.u64 = Get(ctx.r3.u32, kOutputWriteOffset);
}

PPC_FUNC(__imp__XMAGetOutputBufferReadOffset)
{
    (void)base;
    ctx.r3.u64 = Get(ctx.r3.u32, kOutputReadOffset);
}

PPC_FUNC(__imp__XMASetOutputBufferReadOffset)
{
    (void)base;
    Set(ctx.r3.u32, kOutputReadOffset, ctx.r4.u32);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XMAIsOutputBufferValid)
{
    (void)base;
    ctx.r3.u64 = Get(ctx.r3.u32, kOutputValid);
}

PPC_FUNC(__imp__XMASetOutputBufferValid)
{
    (void)base;
    Set(ctx.r3.u32, kOutputValid, 1);
    ctx.r3.u64 = 0;
}
