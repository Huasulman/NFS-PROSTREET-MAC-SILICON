// Funciones de xam.xex implementadas por el runtime. Convención: argumentos en r3..r10, resultado en r3.

#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "imports.h"
#include "objects.h"
#include "threads.h"
#include "../memory.h"
#include "../platform/audio.h"

using namespace kernel;

// --- Notificaciones del sistema --------------------------------------------------------------
// Un "listener" es un evento que está señalado mientras tiene notificaciones pendientes
// (cambios de usuario, de almacenamiento, ventanas del sistema...). Cada notificación se
// entrega a todos los listeners.

namespace
{
    struct Listener : Event
    {
        std::deque<std::pair<uint32_t, uint32_t>> pending; // (id, parámetro), con el cerrojo del dispatcher
        Listener() : Event(true, false) {}
    };

    std::mutex gListenersLock;
    std::vector<std::weak_ptr<Listener>> gListeners;

    constexpr uint32_t kNotifySystemUi = 0x00000009; // XN_SYS_UI: 1 = se abre una ventana del sistema, 0 = se cierra
}

void PostNotification(uint32_t id, uint32_t param)
{
    std::lock_guard listenersLock(gListenersLock);
    for (auto& weak : gListeners)
    {
        if (auto listener = weak.lock())
        {
            std::lock_guard lock(DispatcherLock());
            listener->pending.emplace_back(id, param);
            listener->signaled = true;
            listener->WriteSignalState(1);
        }
    }
    NotifyAll();
}

void ShowSystemUi()
{
    PostNotification(kNotifySystemUi, 1);
    PostNotification(kNotifySystemUi, 0);
}

// uint32_t XamNotifyCreateListener(uint64_t areas, uint32_t maxVersion): devuelve un handle.
PPC_FUNC(__imp__XamNotifyCreateListener)
{
    (void)base;
    auto listener = std::make_shared<Listener>();
    Register(listener);
    {
        std::lock_guard lock(gListenersLock);
        gListeners.push_back(listener);
    }
    if (std::getenv("PROSTREET_TRACE"))
        std::fprintf(stderr, "[xam] XamNotifyCreateListener(áreas 0x%llX)\n", (unsigned long long)ctx.r3.u64);
    ctx.r3.u64 = CreateHandle(listener);
}

// BOOL XNotifyGetNext(uint32_t listener, uint32_t matchId, uint32_t* id, uint32_t* param)
// Con matchId distinto de 0 solo se entrega esa notificación.
PPC_FUNC(__imp__XNotifyGetNext)
{
    (void)base;
    auto object = FromHandle(ctx.r3.u32);
    auto* listener = dynamic_cast<Listener*>(object.get());
    const uint32_t matchId = ctx.r4.u32, idPtr = ctx.r5.u32, paramPtr = ctx.r6.u32;
    ctx.r3.u64 = 0;
    if (!listener)
        return;
    std::lock_guard lock(DispatcherLock());
    for (auto it = listener->pending.begin(); it != listener->pending.end(); ++it)
    {
        if (matchId && it->first != matchId)
            continue;
        if (idPtr)
            memory::Write32(idPtr, it->first);
        if (paramPtr)
            memory::Write32(paramPtr, it->second);
        listener->pending.erase(it);
        ctx.r3.u64 = 1;
        break;
    }
    if (listener->pending.empty())
    {
        listener->signaled = false;
        listener->WriteSignalState(0);
    }
}

// uint32_t XamShowSigninUI(uint32_t panes, uint32_t flags): sin Xbox Live no hay a qué
// conectarse. La ventana se "abre" y se cierra en el acto sin cambiar de usuario.
PPC_FUNC(__imp__XamShowSigninUI)
{
    (void)base;
    ShowSystemUi();
    ctx.r3.u64 = 0;
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
// de audio llama cada vez que necesita más muestras: cada 256 muestras a 48 kHz (5,33 ms). El
// motor de audio del juego (y lo que se sincroniza con él, como los vídeos) avanza con esas
// llamadas. Con salida de sonido, el ritmo lo marca ella (se llama cuando cabe otro bloque);
// sin ella, un temporizador.
PPC_FUNC(__imp__XAudioRegisterRenderDriverClient)
{
    (void)base;
    const uint32_t callback = memory::Read32(ctx.r3.u32), context = memory::Read32(ctx.r3.u32 + 4);
    if (std::getenv("PROSTREET_TRACE"))
        std::fprintf(stderr, "[xam] XAudioRegisterRenderDriverClient(función 0x%08X, contexto 0x%08X, CPU %u)\n", callback,
            context, *static_cast<uint8_t*>(memory::Translate(ctx.r13.u32 + 0x10C)));

    // XAudio guarda datos por procesador (indexados por el número de CPU del KPCR) solo para la
    // CPU en la que se inicializó: la función se llama "en" la CPU del hilo que la registra.
    const uint8_t cpu = *static_cast<uint8_t*>(memory::Translate(ctx.r13.u32 + 0x10C));
    CreateHostThread([callback, context, cpu](PPCContext& thread, uint8_t*) {
        *static_cast<uint8_t*>(memory::Translate(thread.r13.u32 + 0x10C)) = cpu;
        using Clock = std::chrono::steady_clock;
        constexpr auto kPeriod = std::chrono::microseconds(5333); // 256 muestras a 48 kHz
        const bool audio = platform::StartAudio();
        auto next = Clock::now() + kPeriod;
        while (true)
        {
            if (audio)
                platform::WaitForAudioSpace();
            else
                std::this_thread::sleep_until(next);
            thread.r3.u64 = context;
            CallGuest(*CurrentThread(), callback);
            next += kPeriod;
            if (const auto now = Clock::now(); next < now)
                next = now + kPeriod;
        }
    });

    memory::Write32(ctx.r4.u32, 0x41550000); // identificador de driver (como en Xenia)
    ctx.r3.u64 = 0;
}

// NTSTATUS XAudioSubmitRenderDriverFrame(driver, float* samples): un bloque mezclado por el
// juego, 6 canales x 256 muestras en coma flotante big-endian (un canal detrás de otro).
PPC_FUNC(__imp__XAudioSubmitRenderDriverFrame)
{
    (void)base;
    float samples[platform::kAudioFrameSamples * platform::kAudioChannels];
    for (uint32_t i = 0; i < platform::kAudioFrameSamples * platform::kAudioChannels; i++)
    {
        const uint32_t bits = memory::Read32(ctx.r4.u32 + i * 4);
        std::memcpy(&samples[i], &bits, 4);
    }
    platform::SubmitAudioFrame(samples);
    ctx.r3.u64 = 0;
}

// uint32_t XAudioGetVoiceCategoryVolumeChangeMask(driver, uint32_t* mask): sin cambios.
PPC_FUNC(__imp__XAudioGetVoiceCategoryVolumeChangeMask)
{
    (void)base;
    memory::Write32(ctx.r4.u32, 0);
    ctx.r3.u64 = 0;
}

// uint32_t XAudioGetVoiceCategoryVolume(uint32_t category, float* volume): volumen máximo.
PPC_FUNC(__imp__XAudioGetVoiceCategoryVolume)
{
    (void)base;
    const float volume = 1.0f;
    uint32_t bits;
    std::memcpy(&bits, &volume, 4);
    memory::Write32(ctx.r4.u32, bits);
    ctx.r3.u64 = 0;
}

// NTSTATUS XAudioGetSpeakerConfig(uint32_t* config): estéreo, como en Xenia.
PPC_FUNC(__imp__XAudioGetSpeakerConfig)
{
    (void)base;
    memory::Write32(ctx.r3.u32, 0x00010001);
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

// --- Red: sin conexión ------------------------------------------------------------------------
// No hay Xbox Live ni red local: se responde "sin cable" y "sin dirección", para que el juego
// vea enseguida que no hay conexión en lugar de esperar a que se configure.

// uint32_t NetDll_XNetGetTitleXnAddr(uint32_t caller, XNADDR* address): XNET_GET_XNADDR_NONE.
PPC_FUNC(__imp__NetDll_XNetGetTitleXnAddr)
{
    (void)base;
    if (ctx.r4.u32)
        for (uint32_t offset = 0; offset < 36; offset += 4) // XNADDR: 36 bytes
            memory::Write32(ctx.r4.u32 + offset, 0);
    ctx.r3.u64 = 1;
}

// uint32_t NetDll_XNetGetEthernetLinkStatus(uint32_t caller): 0 = sin enlace.
PPC_FUNC(__imp__NetDll_XNetGetEthernetLinkStatus)
{
    (void)base;
    ctx.r3.u64 = 0;
}

// --- Mensajes a aplicaciones del sistema (XMsg) -------------------------------------------------
// La única que usa el juego es el reproductor de música de la consola (XMP, aplicación 0xFA), para
// saber si la música la pone el juego o el usuario (su propia música): si cree que es el usuario,
// apaga la banda sonora.

namespace
{
    constexpr uint32_t kAppXmp = 0xFA;
    constexpr uint32_t kXmpGetPlaybackController = 0x0007001B;
    constexpr uint32_t kXmpControllerTitle = 1; // 0 = usuario, 1 = juego (como Xenia)

    uint32_t DispatchMessage(uint32_t app, uint32_t message, uint32_t buffer)
    {
        if (app == kAppXmp && message == kXmpGetPlaybackController && buffer)
        {
            // {cliente, uint32_t* controlador, uint32_t* bloqueado}
            if (const uint32_t controller = memory::Read32(buffer + 4))
                memory::Write32(controller, kXmpControllerTitle);
            if (const uint32_t locked = memory::Read32(buffer + 8))
                memory::Write32(locked, 0);
        }
        else if (std::getenv("PROSTREET_TRACE"))
        {
            std::fprintf(stderr, "[xam] mensaje 0x%08X a la aplicación 0x%X: se acepta sin más\n", message, app);
        }
        return 0;
    }
}

// uint32_t XMsgInProcessCall(uint32_t app, uint32_t message, void* buffer, uint32_t length)
PPC_FUNC(__imp__XMsgInProcessCall)
{
    (void)base;
    ctx.r3.u64 = DispatchMessage(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
}

// uint32_t XMsgStartIORequestEx(uint32_t app, uint32_t message, XOVERLAPPED* overlapped,
//     void* buffer, uint32_t length, ...)
PPC_FUNC(__imp__XMsgStartIORequestEx)
{
    (void)base;
    const uint32_t result = DispatchMessage(ctx.r3.u32, ctx.r4.u32, ctx.r6.u32);
    if (const uint32_t overlapped = ctx.r5.u32)
    {
        CompleteOverlapped(overlapped, result);
        ctx.r3.u64 = 0x3E5; // ERROR_IO_PENDING
        return;
    }
    ctx.r3.u64 = result;
}

// uint32_t XMsgStartIORequest(uint32_t app, uint32_t message, XOVERLAPPED* overlapped, void* buffer, uint32_t length)
PPC_FUNC(__imp__XMsgStartIORequest)
{
    (void)base;
    const uint32_t result = DispatchMessage(ctx.r3.u32, ctx.r4.u32, ctx.r6.u32);
    if (const uint32_t overlapped = ctx.r5.u32)
    {
        CompleteOverlapped(overlapped, result);
        ctx.r3.u64 = 0x3E5;
        return;
    }
    ctx.r3.u64 = result;
}
