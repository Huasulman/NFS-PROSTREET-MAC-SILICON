// Funciones de xam.xex implementadas por el runtime. Convención: argumentos en r3..r10, resultado en r3.

#include <cstdio>
#include <cstdlib>
#include <vector>

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

// --- Idioma ----------------------------------------------------------------------------------

// uint32_t XGetLanguage(): idioma de la consola.
PPC_FUNC(__imp__XGetLanguage)
{
    (void)base;
    ctx.r3.u64 = ConsoleLanguage();
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

// --- Red: eventos ----------------------------------------------------------------------------
// Los WSAEVENT de la Xbox son eventos normales del kernel (de reinicio manual). Sin red, las
// esperas simplemente no terminan, como en una consola desconectada.

namespace
{
    constexpr uint32_t kWsaWaitTimeout = 0x102;
    constexpr uint32_t kWsaWaitFailed = 0xFFFFFFFF;

    Event* NetEvent(uint32_t handle)
    {
        auto object = FromHandle(handle);
        return object && (object->type == ObjectType::NotificationEvent || object->type == ObjectType::SynchronizationEvent)
                   ? static_cast<Event*>(object.get()) : nullptr;
    }

    bool SetNetEvent(uint32_t handle, bool signaled)
    {
        Event* event = NetEvent(handle);
        if (!event)
            return false;
        {
            std::lock_guard lock(DispatcherLock());
            event->signaled = signaled;
            event->WriteSignalState(signaled);
        }
        if (signaled)
            NotifyAll();
        return true;
    }
}

// WSAEVENT NetDll_WSACreateEvent()
PPC_FUNC(__imp__NetDll_WSACreateEvent)
{
    (void)base;
    ctx.r3.u64 = CreateHandle(Register(std::make_shared<Event>(true, false)));
}

// BOOL NetDll_WSACloseEvent(WSAEVENT event)
PPC_FUNC(__imp__NetDll_WSACloseEvent)
{
    (void)base;
    ctx.r3.u64 = CloseHandle(ctx.r3.u32);
}

// BOOL NetDll_WSASetEvent(WSAEVENT event)
PPC_FUNC(__imp__NetDll_WSASetEvent)
{
    (void)base;
    ctx.r3.u64 = SetNetEvent(ctx.r3.u32, true);
}

// BOOL NetDll_WSAResetEvent(WSAEVENT event)
PPC_FUNC(__imp__NetDll_WSAResetEvent)
{
    (void)base;
    ctx.r3.u64 = SetNetEvent(ctx.r3.u32, false);
}

// uint32_t NetDll_WSAWaitForMultipleEvents(uint32_t count, WSAEVENT* events, BOOL waitAll,
//                                          uint32_t timeoutMs, BOOL alertable)
PPC_FUNC(__imp__NetDll_WSAWaitForMultipleEvents)
{
    (void)base;
    std::vector<std::shared_ptr<Object>> objects;
    for (uint32_t i = 0; i < ctx.r3.u32; i++)
    {
        auto object = FromHandle(memory::Read32(ctx.r4.u32 + i * 4));
        if (!object)
        {
            ctx.r3.u64 = kWsaWaitFailed;
            return;
        }
        objects.push_back(std::move(object));
    }
    const uint32_t timeoutMs = ctx.r6.u32;
    const uint64_t timeout = timeoutMs == 0xFFFFFFFF ? kInfinite : uint64_t(timeoutMs) * 1000000;
    const uint32_t status = Wait(objects, ctx.r5.u32 != 0, timeout);
    ctx.r3.u64 = status == kStatusTimeout ? kWsaWaitTimeout : status; // WSA_WAIT_EVENT_0 + índice
}
