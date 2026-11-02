// Ganchos de depuración sobre funciones del juego: un PPC_FUNC con el nombre de una función
// generada sustituye a su versión débil (PPC_WEAK_FUNC) y puede llamar a la original (__imp__).

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include <ppc_config.h>
#include <ppc_context.h>

extern "C" PPC_FUNC(__imp__sub_824C7600);
extern "C" PPC_FUNC(__imp__sub_824C8138);
#include "../memory.h"

namespace
{
    const bool gFeTrace = std::getenv("PROSTREET_FE_TRACE") != nullptr;
    std::atomic<uint32_t> gFePosts{0};
}

// Publica un mensaje en la cola del motor de menús (FE): r4 = identificador (hash), r7 = tipo.
PPC_FUNC(sub_824C7600)
{
    if (gFeTrace)
    {
        const uint32_t count = gFePosts++;
        if (count < 3000 || count % 100000 == 0)
            std::fprintf(stderr, "[fe] #%u mensaje 0x%08X r5=0x%08X r6=0x%08X tipo %d (lr 0x%08X)\n", count,
                ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, int32_t(ctx.r7.u32), uint32_t(ctx.lr));
    }
    __imp__sub_824C7600(ctx, base);
}

// Ejecuta el guion de un objeto para un mensaje: r4 = manejador (hash en +12), r6 = objeto.
PPC_FUNC(sub_824C8138)
{
    if (gFeTrace)
    {
        const uint32_t count = gFePosts++;
        if (count < 3000)
            std::fprintf(stderr, "[fe] #%u guion 0x%08X objeto 0x%08X (lr 0x%08X)\n", count,
                memory::Read32(ctx.r4.u32 + 12), ctx.r6.u32, uint32_t(ctx.lr));
    }
    __imp__sub_824C8138(ctx, base);
}

// PROSTREET_XMA_TRACE: llamadas a las funciones del decodificador XMA del motor de audio de EA.
extern "C" PPC_FUNC(__imp__sub_8280EE68);
extern "C" PPC_FUNC(__imp__sub_8280F0F0);
extern "C" PPC_FUNC(__imp__sub_828103C8);

namespace
{
    const bool gXmaTrace = std::getenv("PROSTREET_XMA_TRACE") != nullptr;

    void XmaTrace(const char* name, const PPCContext& ctx)
    {
        static std::atomic<int> count{0};
        if (gXmaTrace && count++ < 50)
            std::fprintf(stderr, "[rwac] %s(r3=0x%08X r4=0x%08X r5=0x%08X) lr 0x%08X\n", name, ctx.r3.u32, ctx.r4.u32,
                ctx.r5.u32, uint32_t(ctx.lr));
    }
}

PPC_FUNC(sub_8280EE68) { XmaTrace("inicia_contexto", ctx); __imp__sub_8280EE68(ctx, base); }
PPC_FUNC(sub_8280F0F0) { XmaTrace("sub_8280F0F0", ctx); __imp__sub_8280F0F0(ctx, base); }
PPC_FUNC(sub_828103C8) { XmaTrace("sub_828103C8", ctx); __imp__sub_828103C8(ctx, base); }
