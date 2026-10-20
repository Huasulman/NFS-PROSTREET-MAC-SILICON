#pragma once

// Objetos del kernel (eventos, semáforos, mutantes, temporizadores, hilos) y esperas.
//
// Cada objeto vive en el anfitrión, pero tiene además una cabecera DISPATCHER_HEADER en la
// memoria del invitado: el juego usa tanto handles (funciones Nt*) como punteros a esa cabecera
// (funciones Ke*), y las dos vías llevan al mismo objeto. Los objetos que el juego inicializa en
// su propia memoria (KeInitializeSemaphore, o eventos dentro de sus estructuras) se crean al
// primer uso a partir de su cabecera.
//
// Todas las esperas y señales se sincronizan con un único cerrojo (como el dispatcher de Windows).

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace kernel
{
    // Tipos de DISPATCHER_HEADER.Type del kernel de la Xbox 360.
    enum class ObjectType : uint8_t
    {
        NotificationEvent = 0,       // evento de reinicio manual
        SynchronizationEvent = 1,    // evento de reinicio automático
        Mutant = 2,
        Semaphore = 5,
        Thread = 6,
        NotificationTimer = 8,
        SynchronizationTimer = 9,
    };

    struct Thread;

    struct Object
    {
        ObjectType type;
        uint32_t guest = 0; // dirección de su DISPATCHER_HEADER en el invitado

        explicit Object(ObjectType type) : type(type) {}
        virtual ~Object() = default;

        // Con el cerrojo del dispatcher tomado:
        virtual bool IsSignaled(Thread* waiter) const = 0;
        virtual void Acquire(Thread* waiter) { (void)waiter; } // efecto de una espera satisfecha
        virtual std::chrono::steady_clock::time_point NextChange() const { return std::chrono::steady_clock::time_point::max(); }

        void WriteSignalState(int32_t state) const; // refleja el estado en la cabecera del invitado
    };

    struct Event : Object
    {
        bool signaled;
        Event(bool manualReset, bool initialState)
            : Object(manualReset ? ObjectType::NotificationEvent : ObjectType::SynchronizationEvent), signaled(initialState) {}
        bool IsSignaled(Thread*) const override { return signaled; }
        void Acquire(Thread*) override;
    };

    struct Semaphore : Object
    {
        int32_t count, limit;
        Semaphore(int32_t count, int32_t limit) : Object(ObjectType::Semaphore), count(count), limit(limit) {}
        bool IsSignaled(Thread*) const override { return count > 0; }
        void Acquire(Thread*) override;
    };

    struct Mutant : Object
    {
        Thread* owner = nullptr;
        uint32_t recursion = 0;
        Mutant() : Object(ObjectType::Mutant) {}
        bool IsSignaled(Thread* waiter) const override { return owner == nullptr || owner == waiter; }
        void Acquire(Thread* waiter) override;
    };

    struct Timer : Object
    {
        std::chrono::steady_clock::time_point due = std::chrono::steady_clock::time_point::max();
        std::chrono::nanoseconds period{0};
        bool signaled = false;
        explicit Timer(bool manualReset) : Object(manualReset ? ObjectType::NotificationTimer : ObjectType::SynchronizationTimer) {}
        bool IsSignaled(Thread*) const override;
        void Acquire(Thread*) override;
        std::chrono::steady_clock::time_point NextChange() const override { return due; }
    };

    // --- Dispatcher ------------------------------------------------------------------------

    std::mutex& DispatcherLock();
    void NotifyAll(); // despierta a todos los que esperan para que vuelvan a comprobar

    // Registra un objeto: le da una cabecera en el invitado si no la tiene y lo hace visible
    // por su puntero. Devuelve el mismo objeto.
    std::shared_ptr<Object> Register(std::shared_ptr<Object> object);

    // Objeto por puntero a su cabecera. Si no existe, lo crea a partir de la cabecera.
    std::shared_ptr<Object> FromGuest(uint32_t header);

    // --- Handles ---------------------------------------------------------------------------

    constexpr uint32_t kCurrentProcess = 0xFFFFFFFF;
    constexpr uint32_t kCurrentThread = 0xFFFFFFFE;

    uint32_t CreateHandle(std::shared_ptr<Object> object);
    std::shared_ptr<Object> FromHandle(uint32_t handle); // nullptr si no es válido
    bool CloseHandle(uint32_t handle);

    // --- Esperas ---------------------------------------------------------------------------

    constexpr uint32_t kStatusTimeout = 0x00000102;
    constexpr uint64_t kInfinite = ~0ull;

    // Convierte un tiempo de espera del kernel (puntero a int64 en unidades de 100 ns: negativo =
    // relativo, positivo = absoluto) en nanosegundos de espera. Puntero nulo = infinito.
    uint64_t TimeoutFromGuest(uint32_t timeoutPtr);

    // Espera a uno (waitAll = false) o a todos los objetos. Devuelve STATUS_SUCCESS + índice o
    // STATUS_TIMEOUT.
    uint32_t Wait(const std::vector<std::shared_ptr<Object>>& objects, bool waitAll, uint64_t timeoutNs);
}
