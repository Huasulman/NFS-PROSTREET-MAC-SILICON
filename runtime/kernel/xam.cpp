// Funciones de xam.xex implementadas por el runtime. Convención: argumentos en r3..r10, resultado en r3.

#include <cstdio>
#include <cstdlib>

#include "imports.h"
#include "objects.h"
#include "../memory.h"

using namespace kernel;

// --- Notificaciones del sistema --------------------------------------------------------------
// Un "listener" es un evento que se señala cuando hay notificaciones (cambios de usuario, de
// almacenamiento, de la interfaz...). De momento no se genera ninguna.

// uint32_t XamNotifyCreateListener(uint64_t areas, uint32_t maxVersion): devuelve un handle.
PPC_FUNC(__imp__XamNotifyCreateListener)
{
    (void)base;
    auto listener = Register(std::make_shared<Event>(true, false));
    if (std::getenv("PROSTREET_TRACE"))
        std::fprintf(stderr, "[xam] XamNotifyCreateListener(áreas 0x%llX)\n", (unsigned long long)ctx.r3.u64);
    ctx.r3.u64 = CreateHandle(listener);
}

// BOOL XNotifyGetNext(uint32_t listener, uint32_t matchId, uint32_t* id, uint32_t* param)
PPC_FUNC(__imp__XNotifyGetNext)
{
    (void)base;
    ctx.r3.u64 = 0; // no hay notificaciones pendientes
}

// --- Audio -----------------------------------------------------------------------------------

// NTSTATUS XAudioRegisterRenderDriverClient(XAUDIO_CALLBACK* callback, uint32_t* driver)
// El cliente es una función del juego (callback[0]) con su contexto (callback[1]) que el sistema
// de audio llama cada vez que necesita más muestras. De momento solo se registra: no hay salida
// de audio, así que no se llama nunca.
PPC_FUNC(__imp__XAudioRegisterRenderDriverClient)
{
    (void)base;
    const uint32_t callback = memory::Read32(ctx.r3.u32), context = memory::Read32(ctx.r3.u32 + 4);
    if (std::getenv("PROSTREET_TRACE"))
        std::fprintf(stderr, "[xam] XAudioRegisterRenderDriverClient(función 0x%08X, contexto 0x%08X)\n", callback, context);
    memory::Write32(ctx.r4.u32, 0x41550000); // identificador de driver (como en Xenia)
    ctx.r3.u64 = 0;
}
