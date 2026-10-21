#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <pthread.h>

#include <ppc_config.h>
#include <ppc_context.h>

#include "objects.h"

namespace kernel
{
    // Un hilo del juego: un hilo del anfitrión con su propio contexto PowerPC, pila del invitado,
    // KPCR (al que apunta r13), TLS y KTHREAD. Su cabecera en el invitado es el KTHREAD.
    struct Thread : Object
    {
        uint32_t id = 0;
        uint32_t pcr = 0;
        uint32_t stackBase = 0, stackEnd = 0;
        uint32_t startAddress = 0, startContext = 0, xapiStartup = 0;
        uint32_t suspendCount = 0; // con el cerrojo del dispatcher
        bool exited = false;
        uint32_t exitCode = 0;
        int32_t priority = 0;
        uint32_t affinity = 0;
        pthread_t host{};
        PPCContext* ctx = nullptr;
        std::function<void(PPCContext&, uint8_t*)> hostBody; // hilos del runtime (p. ej. la GPU)

        Thread() : Object(ObjectType::Thread) {}
        ~Thread() override;
        bool IsSignaled(Thread*) const override { return exited; }
    };

    // Lanzada por ExTerminateThread para salir del hilo desde dentro del código del juego.
    struct ThreadExit
    {
        uint32_t code;
    };

    Thread* CurrentThread();
    std::shared_ptr<Object> CurrentThreadObject();
    std::shared_ptr<Thread> FindThreadById(uint32_t id);

    // Crea un hilo del juego. Si `xapiStartup` no es 0, el hilo ejecuta xapiStartup(start, context)
    // como hace el kernel de la Xbox; si no, start(context).
    std::shared_ptr<Thread> CreateThread(uint32_t startAddress, uint32_t startContext, uint32_t xapiStartup,
        uint32_t stackSize, bool suspended);

    // Crea un hilo del runtime que ejecuta `body` en el anfitrión, con un contexto de invitado
    // completo (pila, KPCR, TLS) para poder llamar a funciones del juego.
    std::shared_ptr<Thread> CreateHostThread(std::function<void(PPCContext&, uint8_t*)> body);

    // Llama a una función del juego desde un hilo del runtime, con la pila reiniciada.
    void CallGuest(Thread& thread, uint32_t function);

    // Reanuda un hilo; devuelve el contador de suspensión anterior.
    uint32_t ResumeThread(Thread& thread);

    // Tiempo del sistema como FILETIME (unidades de 100 ns desde 1601).
    int64_t SystemTime();
}
