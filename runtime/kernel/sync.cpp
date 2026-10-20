// Sincronización del kernel: eventos, semáforos, mutantes, temporizadores, esperas, secciones
// críticas, spinlocks y tiempo. Convención: argumentos en r3..r10, resultado en r3.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "imports.h"
#include "objects.h"
#include "threads.h"
#include "../memory.h"

using namespace kernel;

namespace
{
    constexpr uint32_t kStatusSuccess = 0x00000000;
    constexpr uint32_t kStatusInvalidHandle = 0xC0000008;
    constexpr uint32_t kStatusSemaphoreLimitExceeded = 0xC0000047;
    constexpr uint32_t kStatusMutantNotOwned = 0xC0000046;

    template <typename T>
    T* As(const std::shared_ptr<Object>& object, ObjectType a, ObjectType b)
    {
        return object && (object->type == a || object->type == b) ? static_cast<T*>(object.get()) : nullptr;
    }

    Event* AsEvent(const std::shared_ptr<Object>& o) { return As<Event>(o, ObjectType::NotificationEvent, ObjectType::SynchronizationEvent); }
    Semaphore* AsSemaphore(const std::shared_ptr<Object>& o) { return As<Semaphore>(o, ObjectType::Semaphore, ObjectType::Semaphore); }
    Mutant* AsMutant(const std::shared_ptr<Object>& o) { return As<Mutant>(o, ObjectType::Mutant, ObjectType::Mutant); }
    Timer* AsTimer(const std::shared_ptr<Object>& o) { return As<Timer>(o, ObjectType::NotificationTimer, ObjectType::SynchronizationTimer); }

    void WriteOptional(uint32_t pointer, uint32_t value)
    {
        if (pointer)
            memory::Write32(pointer, value);
    }

    // Cambia el estado de un evento y devuelve el anterior.
    uint32_t SetEvent(Event& event, bool signaled)
    {
        uint32_t previous;
        {
            std::lock_guard lock(DispatcherLock());
            previous = event.signaled;
            event.signaled = signaled;
            event.WriteSignalState(signaled);
        }
        if (signaled)
            NotifyAll();
        return previous;
    }

    // "Pulso": despierta a quien espere ahora y deja el evento sin señalar.
    uint32_t PulseEvent(Event& event)
    {
        const uint32_t previous = SetEvent(event, true);
        std::this_thread::yield();
        SetEvent(event, false);
        return previous;
    }

    // Suma `count` al semáforo; devuelve el valor anterior o -1 si supera el límite.
    int32_t ReleaseSemaphore(Semaphore& semaphore, int32_t count)
    {
        int32_t previous;
        {
            std::lock_guard lock(DispatcherLock());
            previous = semaphore.count;
            if (semaphore.count + count > semaphore.limit)
                return -1;
            semaphore.count += count;
            semaphore.WriteSignalState(semaphore.count);
        }
        NotifyAll();
        return previous;
    }
}

// --- Eventos -------------------------------------------------------------------------------

// NTSTATUS NtCreateEvent(uint32_t* handle, void* attributes, uint32_t type, uint32_t initialState)
// type: 0 = notificación (reinicio manual), 1 = sincronización (reinicio automático).
PPC_FUNC(__imp__NtCreateEvent)
{
    (void)base;
    auto event = Register(std::make_shared<Event>(ctx.r5.u32 == 0, ctx.r6.u32 != 0));
    memory::Write32(ctx.r3.u32, CreateHandle(event));
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtSetEvent(uint32_t handle, int32_t* previousState)
PPC_FUNC(__imp__NtSetEvent)
{
    (void)base;
    Event* event = AsEvent(FromHandle(ctx.r3.u32));
    if (!event) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    WriteOptional(ctx.r4.u32, SetEvent(*event, true));
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtClearEvent(uint32_t handle)
PPC_FUNC(__imp__NtClearEvent)
{
    (void)base;
    Event* event = AsEvent(FromHandle(ctx.r3.u32));
    if (!event) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    SetEvent(*event, false);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtPulseEvent(uint32_t handle, int32_t* previousState)
PPC_FUNC(__imp__NtPulseEvent)
{
    (void)base;
    Event* event = AsEvent(FromHandle(ctx.r3.u32));
    if (!event) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    WriteOptional(ctx.r4.u32, PulseEvent(*event));
    ctx.r3.u64 = kStatusSuccess;
}

// int32_t KeSetEvent(KEVENT* event, int32_t increment, uint32_t wait): devuelve el estado anterior.
PPC_FUNC(__imp__KeSetEvent)
{
    (void)base;
    Event* event = AsEvent(FromGuest(ctx.r3.u32));
    ctx.r3.u64 = event ? SetEvent(*event, true) : 0;
}

// int32_t KeResetEvent(KEVENT* event)
PPC_FUNC(__imp__KeResetEvent)
{
    (void)base;
    Event* event = AsEvent(FromGuest(ctx.r3.u32));
    ctx.r3.u64 = event ? SetEvent(*event, false) : 0;
}

// int32_t KePulseEvent(KEVENT* event, int32_t increment, uint32_t wait)
PPC_FUNC(__imp__KePulseEvent)
{
    (void)base;
    Event* event = AsEvent(FromGuest(ctx.r3.u32));
    ctx.r3.u64 = event ? PulseEvent(*event) : 0;
}

// --- Semáforos -----------------------------------------------------------------------------

// NTSTATUS NtCreateSemaphore(uint32_t* handle, void* attributes, int32_t initialCount, int32_t maximumCount)
PPC_FUNC(__imp__NtCreateSemaphore)
{
    (void)base;
    auto semaphore = Register(std::make_shared<Semaphore>(ctx.r5.s32, ctx.r6.s32));
    static_cast<Semaphore&>(*semaphore).WriteSignalState(ctx.r5.s32);
    memory::Write32(ctx.r3.u32, CreateHandle(semaphore));
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtReleaseSemaphore(uint32_t handle, int32_t releaseCount, int32_t* previousCount)
PPC_FUNC(__imp__NtReleaseSemaphore)
{
    (void)base;
    Semaphore* semaphore = AsSemaphore(FromHandle(ctx.r3.u32));
    if (!semaphore) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    const int32_t previous = ReleaseSemaphore(*semaphore, ctx.r4.s32);
    if (previous < 0) { ctx.r3.u64 = kStatusSemaphoreLimitExceeded; return; }
    WriteOptional(ctx.r5.u32, uint32_t(previous));
    ctx.r3.u64 = kStatusSuccess;
}

// void KeInitializeSemaphore(KSEMAPHORE* semaphore, int32_t count, int32_t limit)
PPC_FUNC(__imp__KeInitializeSemaphore)
{
    (void)base;
    const uint32_t header = ctx.r3.u32;
    std::memset(memory::Translate(header), 0, 0x14);
    *static_cast<uint8_t*>(memory::Translate(header)) = uint8_t(ObjectType::Semaphore);
    memory::Write32(header + 0x10, ctx.r5.u32); // Limit
    auto semaphore = std::make_shared<Semaphore>(ctx.r4.s32, ctx.r5.s32);
    semaphore->guest = header;
    Register(semaphore);
    semaphore->WriteSignalState(ctx.r4.s32);
}

// int32_t KeReleaseSemaphore(KSEMAPHORE* semaphore, int32_t increment, int32_t adjustment, uint32_t wait)
PPC_FUNC(__imp__KeReleaseSemaphore)
{
    (void)base;
    Semaphore* semaphore = AsSemaphore(FromGuest(ctx.r3.u32));
    ctx.r3.u64 = semaphore ? uint64_t(int64_t(ReleaseSemaphore(*semaphore, ctx.r5.s32))) : 0;
}

// --- Mutantes ------------------------------------------------------------------------------

// NTSTATUS NtCreateMutant(uint32_t* handle, void* attributes, uint32_t initialOwner)
PPC_FUNC(__imp__NtCreateMutant)
{
    (void)base;
    auto mutant = std::make_shared<Mutant>();
    if (ctx.r5.u32)
    {
        mutant->owner = CurrentThread();
        mutant->recursion = 1;
    }
    Register(mutant);
    mutant->WriteSignalState(mutant->owner ? 0 : 1);
    memory::Write32(ctx.r3.u32, CreateHandle(mutant));
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtReleaseMutant(uint32_t handle, int32_t* previousCount)
PPC_FUNC(__imp__NtReleaseMutant)
{
    (void)base;
    Mutant* mutant = AsMutant(FromHandle(ctx.r3.u32));
    if (!mutant) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    {
        std::lock_guard lock(DispatcherLock());
        if (mutant->owner != CurrentThread()) { ctx.r3.u64 = kStatusMutantNotOwned; return; }
        WriteOptional(ctx.r4.u32, mutant->recursion);
        if (--mutant->recursion == 0)
        {
            mutant->owner = nullptr;
            mutant->WriteSignalState(1);
        }
    }
    NotifyAll();
    ctx.r3.u64 = kStatusSuccess;
}

// --- Temporizadores ------------------------------------------------------------------------

// NTSTATUS NtCreateTimer(uint32_t* handle, void* attributes, uint32_t type)
// type: 0 = notificación, 1 = sincronización.
PPC_FUNC(__imp__NtCreateTimer)
{
    (void)base;
    auto timer = Register(std::make_shared<Timer>(ctx.r5.u32 == 0));
    memory::Write32(ctx.r3.u32, CreateHandle(timer));
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtSetTimerEx(uint32_t handle, int64_t* dueTime, void* apcRoutine, uint32_t unknownOne,
//                       void* apcContext, uint32_t resume, int32_t periodMs, uint32_t unknownZero)
// Las rutinas APC del temporizador no se ejecutan.
PPC_FUNC(__imp__NtSetTimerEx)
{
    (void)base;
    Timer* timer = AsTimer(FromHandle(ctx.r3.u32));
    if (!timer) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    if (ctx.r5.u32)
        std::fprintf(stderr, "[kernel] NtSetTimerEx con rutina APC 0x%08X: no se ejecutará\n", ctx.r5.u32);
    const uint64_t delay = TimeoutFromGuest(ctx.r4.u32);
    {
        std::lock_guard lock(DispatcherLock());
        timer->signaled = false;
        timer->due = std::chrono::steady_clock::now() + std::chrono::nanoseconds(delay == kInfinite ? 0 : delay);
        timer->period = std::chrono::milliseconds(ctx.r9.s32); // el periodo va en milisegundos
        timer->WriteSignalState(0);
    }
    NotifyAll();
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtCancelTimer(uint32_t handle, uint32_t* currentState)
PPC_FUNC(__imp__NtCancelTimer)
{
    (void)base;
    Timer* timer = AsTimer(FromHandle(ctx.r3.u32));
    if (!timer) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    std::lock_guard lock(DispatcherLock());
    WriteOptional(ctx.r4.u32, timer->IsSignaled(nullptr));
    timer->due = std::chrono::steady_clock::time_point::max();
    timer->period = std::chrono::nanoseconds(0);
    ctx.r3.u64 = kStatusSuccess;
}

// --- Esperas -------------------------------------------------------------------------------

// NTSTATUS KeWaitForSingleObject(void* object, uint32_t reason, uint32_t mode, uint32_t alertable, int64_t* timeout)
PPC_FUNC(__imp__KeWaitForSingleObject)
{
    (void)base;
    ctx.r3.u64 = Wait({ FromGuest(ctx.r3.u32) }, false, TimeoutFromGuest(ctx.r7.u32));
}

// NTSTATUS NtWaitForSingleObjectEx(uint32_t handle, uint32_t mode, uint32_t alertable, int64_t* timeout)
PPC_FUNC(__imp__NtWaitForSingleObjectEx)
{
    (void)base;
    auto object = FromHandle(ctx.r3.u32);
    if (!object) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    ctx.r3.u64 = Wait({ object }, false, TimeoutFromGuest(ctx.r6.u32));
}

// NTSTATUS KeWaitForMultipleObjects(uint32_t count, void** objects, uint32_t waitType, uint32_t reason,
//                                   uint32_t mode, uint32_t alertable, int64_t* timeout, void* waitBlocks)
// waitType: 0 = todos, 1 = cualquiera.
PPC_FUNC(__imp__KeWaitForMultipleObjects)
{
    (void)base;
    std::vector<std::shared_ptr<Object>> objects;
    for (uint32_t i = 0; i < ctx.r3.u32; i++)
        objects.push_back(FromGuest(memory::Read32(ctx.r4.u32 + i * 4)));
    ctx.r3.u64 = Wait(objects, ctx.r5.u32 == 0, TimeoutFromGuest(ctx.r9.u32));
}

// NTSTATUS NtWaitForMultipleObjectsEx(uint32_t count, uint32_t* handles, uint32_t waitType,
//                                     uint32_t mode, uint32_t alertable, int64_t* timeout)
PPC_FUNC(__imp__NtWaitForMultipleObjectsEx)
{
    (void)base;
    std::vector<std::shared_ptr<Object>> objects;
    for (uint32_t i = 0; i < ctx.r3.u32; i++)
    {
        auto object = FromHandle(memory::Read32(ctx.r4.u32 + i * 4));
        if (!object) { ctx.r3.u64 = kStatusInvalidHandle; return; }
        objects.push_back(std::move(object));
    }
    ctx.r3.u64 = Wait(objects, ctx.r5.u32 == 0, TimeoutFromGuest(ctx.r8.u32));
}

// NTSTATUS NtSignalAndWaitForSingleObjectEx(uint32_t signal, uint32_t wait, uint32_t mode,
//                                           uint32_t alertable, int64_t* timeout)
PPC_FUNC(__imp__NtSignalAndWaitForSingleObjectEx)
{
    (void)base;
    auto signal = FromHandle(ctx.r3.u32);
    auto wait = FromHandle(ctx.r4.u32);
    if (!signal || !wait) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    if (Event* event = AsEvent(signal))
        SetEvent(*event, true);
    else if (Semaphore* semaphore = AsSemaphore(signal))
        ReleaseSemaphore(*semaphore, 1);
    else if (Mutant* mutant = AsMutant(signal))
    {
        std::lock_guard lock(DispatcherLock());
        if (mutant->owner == CurrentThread() && --mutant->recursion == 0)
            mutant->owner = nullptr;
    }
    NotifyAll();
    ctx.r3.u64 = Wait({ wait }, false, TimeoutFromGuest(ctx.r7.u32));
}

// --- Secciones críticas --------------------------------------------------------------------
// Cada sección crítica del invitado tiene un mutex recursivo del anfitrión, por su dirección.
// Se crea al primer uso (algunas están inicializadas estáticamente en .data).

namespace
{
    std::mutex gCriticalSectionsLock;
    std::unordered_map<uint32_t, std::recursive_mutex*> gCriticalSections;

    std::recursive_mutex& CriticalSection(uint32_t address)
    {
        std::lock_guard lock(gCriticalSectionsLock);
        auto& mutex = gCriticalSections[address];
        if (!mutex)
            mutex = new std::recursive_mutex(); // vive lo que dure el programa
        return *mutex;
    }
}

// void RtlInitializeCriticalSection(RTL_CRITICAL_SECTION* cs)
PPC_FUNC(__imp__RtlInitializeCriticalSection)
{
    (void)base;
    CriticalSection(ctx.r3.u32);
}

// void RtlEnterCriticalSection(RTL_CRITICAL_SECTION* cs)
PPC_FUNC(__imp__RtlEnterCriticalSection)
{
    (void)base;
    CriticalSection(ctx.r3.u32).lock();
}

// void RtlLeaveCriticalSection(RTL_CRITICAL_SECTION* cs)
PPC_FUNC(__imp__RtlLeaveCriticalSection)
{
    (void)base;
    CriticalSection(ctx.r3.u32).unlock();
}

// BOOL RtlTryEnterCriticalSection(RTL_CRITICAL_SECTION* cs)
PPC_FUNC(__imp__RtlTryEnterCriticalSection)
{
    (void)base;
    ctx.r3.u64 = CriticalSection(ctx.r3.u32).try_lock();
}

// --- Spinlocks e IRQL ----------------------------------------------------------------------
// El spinlock es una palabra de 32 bits del invitado: 0 = libre; si no, el KPCR del dueño.

namespace
{
    bool TryAcquireSpinLock(const PPCContext& ctx, uint32_t lock)
    {
        uint32_t expected = 0;
        auto* word = static_cast<uint32_t*>(memory::Translate(lock));
        return __atomic_compare_exchange_n(word, &expected, __builtin_bswap32(ctx.r13.u32), false,
            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
    }

    void AcquireSpinLock(const PPCContext& ctx, uint32_t lock)
    {
        while (!TryAcquireSpinLock(ctx, lock))
            std::this_thread::yield();
    }

    void ReleaseSpinLock(uint32_t lock)
    {
        __atomic_store_n(static_cast<uint32_t*>(memory::Translate(lock)), 0u, __ATOMIC_RELEASE);
    }
}

// uint32_t KfAcquireSpinLock(uint32_t* lock): devuelve el IRQL anterior (siempre 0 aquí).
PPC_FUNC(__imp__KfAcquireSpinLock)
{
    (void)base;
    AcquireSpinLock(ctx, ctx.r3.u32);
    ctx.r3.u64 = 0;
}

// void KfReleaseSpinLock(uint32_t* lock, uint32_t oldIrql)
PPC_FUNC(__imp__KfReleaseSpinLock)
{
    (void)base;
    ReleaseSpinLock(ctx.r3.u32);
}

// void KeAcquireSpinLockAtRaisedIrql(uint32_t* lock)
PPC_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql)
{
    (void)base;
    AcquireSpinLock(ctx, ctx.r3.u32);
}

// void KeReleaseSpinLockFromRaisedIrql(uint32_t* lock)
PPC_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql)
{
    (void)base;
    ReleaseSpinLock(ctx.r3.u32);
}

// BOOL KeTryToAcquireSpinLockAtRaisedIrql(uint32_t* lock)
PPC_FUNC(__imp__KeTryToAcquireSpinLockAtRaisedIrql)
{
    (void)base;
    ctx.r3.u64 = TryAcquireSpinLock(ctx, ctx.r3.u32);
}

// uint32_t KeRaiseIrqlToDpcLevel(): el IRQL no se modela; devuelve 0 (PASSIVE_LEVEL).
PPC_FUNC(__imp__KeRaiseIrqlToDpcLevel)
{
    (void)base;
    ctx.r3.u64 = 0;
}

// void KfLowerIrql(uint32_t irql)
PPC_FUNC(__imp__KfLowerIrql)
{
    (void)base;
}

// --- Tiempo --------------------------------------------------------------------------------

// void KeQuerySystemTime(int64_t* time)
PPC_FUNC(__imp__KeQuerySystemTime)
{
    (void)base;
    const uint64_t now = uint64_t(SystemTime());
    memory::Write32(ctx.r3.u32, uint32_t(now >> 32));
    memory::Write32(ctx.r3.u32 + 4, uint32_t(now));
}

// uint64_t KeQueryPerformanceFrequency()
// El código recompilado lee el contador del procesador (mftb) con el contador del anfitrión,
// así que la frecuencia debe ser la del anfitrión (24 MHz en Apple Silicon), no los 50 MHz de la Xbox.
PPC_FUNC(__imp__KeQueryPerformanceFrequency)
{
    (void)base;
    uint64_t frequency;
#if defined(__aarch64__)
    asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
#else
    frequency = 50000000;
#endif
    ctx.r3.u64 = frequency;
}
