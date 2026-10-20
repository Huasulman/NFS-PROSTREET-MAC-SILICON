#include "threads.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <thread>
#include <unordered_map>

#include "imports.h"
#include "xex.h"
#include "../memory.h"

namespace kernel
{
    namespace
    {
        constexpr uint32_t kTlsTemplate = 0x8309CA00; // sección .tls
        constexpr uint32_t kTlsTemplateSize = 0x8;
        constexpr uint32_t kTlsDynamicSlots = 64;     // ver KeTls* en xboxkrnl.cpp
        constexpr uint32_t kMinStackSize = 64 * 1024;
        constexpr uint32_t kKThreadSize = 0x1000;
        constexpr size_t kHostStackSize = 64 * 1024 * 1024;
        constexpr uint32_t kCreateSuspended = 0x1;

        // Desplazamientos del KPCR al que apunta r13 (según Xenia, X_KPCR).
        constexpr uint32_t kPcrTlsPtr = 0x00;
        constexpr uint32_t kPcrSelfPtr = 0x30;
        constexpr uint32_t kPcrStackBase = 0x70;
        constexpr uint32_t kPcrStackEnd = 0x74;
        constexpr uint32_t kPcrCurrentThread = 0x100;

        thread_local Thread* tCurrent = nullptr;
        thread_local std::shared_ptr<Thread> tCurrentRef;

        std::mutex gThreadsLock;
        std::unordered_map<uint32_t, std::weak_ptr<Thread>> gThreadsById;
        std::atomic<uint32_t> gNextThreadId{1};

        void* ThreadMain(void* argument)
        {
            // Toma la referencia que dejó CreateThread: el objeto vive al menos lo que dure el hilo.
            std::shared_ptr<Thread> self(std::move(*static_cast<std::shared_ptr<Thread>*>(argument)));
            delete static_cast<std::shared_ptr<Thread>*>(argument);
            tCurrent = self.get();
            tCurrentRef = self;

            // Pila propia para el manejador de errores (ver main.cpp).
            stack_t signalStack{};
            signalStack.ss_size = 1024 * 1024;
            signalStack.ss_sp = std::malloc(signalStack.ss_size);
            sigaltstack(&signalStack, nullptr);

            // Hilo creado suspendido: esperar a NtResumeThread / KeResumeThread.
            {
                std::unique_lock lock(DispatcherLock());
                while (self->suspendCount > 0)
                {
                    lock.unlock();
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                    lock.lock();
                }
            }

            PPCContext& ctx = *self->ctx;
            uint8_t* base = memory::Base();
            uint32_t function;
            if (self->xapiStartup != 0)
            {
                ctx.r3.u64 = self->startAddress;
                ctx.r4.u64 = self->startContext;
                function = self->xapiStartup;
            }
            else
            {
                ctx.r3.u64 = self->startContext;
                function = self->startAddress;
            }

            if (std::getenv("PROSTREET_TRACE"))
                std::fprintf(stderr, "[kernel] hilo %u arranca en 0x%08X\n", self->id, function);
            try
            {
                (PPC_LOOKUP_FUNC(base, function))(ctx, base);
                self->exitCode = ctx.r3.u32;
            }
            catch (const ThreadExit& exit)
            {
                self->exitCode = exit.code;
            }
            if (std::getenv("PROSTREET_TRACE"))
                std::fprintf(stderr, "[kernel] hilo %u termina (código 0x%X)\n", self->id, self->exitCode);

            {
                std::lock_guard lock(DispatcherLock());
                self->exited = true;
                self->WriteSignalState(1);
            }
            NotifyAll();
            tCurrent = nullptr;
            tCurrentRef.reset();
            return nullptr;
        }
    }

    Thread::~Thread()
    {
        std::free(ctx);
    }

    Thread* CurrentThread() { return tCurrent; }

    std::shared_ptr<Object> CurrentThreadObject() { return tCurrentRef; }

    std::shared_ptr<Thread> FindThreadById(uint32_t id)
    {
        std::lock_guard lock(gThreadsLock);
        auto it = gThreadsById.find(id);
        return it == gThreadsById.end() ? nullptr : it->second.lock();
    }

    std::shared_ptr<Thread> CreateThread(uint32_t startAddress, uint32_t startContext, uint32_t xapiStartup,
        uint32_t stackSize, bool suspended)
    {
        auto thread = std::make_shared<Thread>();
        thread->id = gNextThreadId++;
        thread->startAddress = startAddress;
        thread->startContext = startContext;
        thread->xapiStartup = xapiStartup;
        thread->suspendCount = suspended ? 1 : 0;

        // KTHREAD: es la cabecera del objeto en el invitado (lo que devuelve ObReferenceObjectByHandle).
        thread->guest = memory::AllocateKernel(kKThreadSize, 0x10);
        Register(thread);

        // Pila del invitado (crece hacia abajo).
        stackSize = std::max(kMinStackSize, (stackSize + 0xFFF) & ~0xFFFu);
        thread->stackEnd = memory::AllocateKernel(stackSize, 0x1000);
        thread->stackBase = thread->stackEnd + stackSize;

        // TLS: slots dinámicos (KeTlsAlloc) seguidos de la copia de la plantilla .tls.
        const uint32_t tls = memory::AllocateKernel(kTlsDynamicSlots * 4 + 0x100, 0x10);
        const uint32_t tlsStatic = tls + kTlsDynamicSlots * 4;
        std::memcpy(memory::Translate(tlsStatic), memory::Translate(kTlsTemplate), kTlsTemplateSize);

        thread->pcr = memory::AllocateKernel(0x1000, 0x1000);
        memory::Write32(thread->pcr + kPcrTlsPtr, tlsStatic);
        memory::Write32(thread->pcr + kPcrSelfPtr, thread->pcr);
        memory::Write32(thread->pcr + kPcrStackBase, thread->stackBase);
        memory::Write32(thread->pcr + kPcrStackEnd, thread->stackEnd);
        memory::Write32(thread->pcr + kPcrCurrentThread, thread->guest);

        thread->ctx = static_cast<PPCContext*>(std::aligned_alloc(alignof(PPCContext), (sizeof(PPCContext) + 63) & ~size_t(63)));
        new (thread->ctx) PPCContext{};
        thread->ctx->r1.u64 = thread->stackBase - 0x100;
        thread->ctx->r13.u64 = thread->pcr;
        thread->ctx->fpscr.loadFromHost();

        {
            std::lock_guard lock(gThreadsLock);
            gThreadsById[thread->id] = thread;
        }

        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, kHostStackSize);
        auto* argument = new std::shared_ptr<Thread>(thread);
        if (pthread_create(&thread->host, &attributes, ThreadMain, argument) != 0)
        {
            delete argument;
            std::fprintf(stderr, "[kernel] no se pudo crear el hilo del anfitrión\n");
        }
        pthread_attr_destroy(&attributes);
        return thread;
    }

    uint32_t ResumeThread(Thread& thread)
    {
        std::lock_guard lock(DispatcherLock());
        const uint32_t previous = thread.suspendCount;
        if (thread.suspendCount > 0)
            thread.suspendCount--;
        return previous;
    }

    int64_t SystemTime()
    {
        // FILETIME: 100 ns desde el 1 de enero de 1601.
        constexpr int64_t kUnixEpochAsFileTime = 116444736000000000LL;
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return kUnixEpochAsFileTime + std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 100;
    }
}

using namespace kernel;

namespace
{
    constexpr uint32_t kStatusSuccess = 0x00000000;
    constexpr uint32_t kStatusInvalidHandle = 0xC0000008;
    constexpr uint32_t kStatusNoMemory = 0xC0000017;

    Thread* ThreadFromGuest(uint32_t kthread)
    {
        auto object = FromGuest(kthread);
        return object && object->type == ObjectType::Thread ? static_cast<Thread*>(object.get()) : nullptr;
    }
}

// NTSTATUS ExCreateThread(uint32_t* handle, uint32_t stackSize, uint32_t* threadId,
//                         uint32_t xapiStartup, uint32_t startAddress, uint32_t startContext, uint32_t flags)
PPC_FUNC(__imp__ExCreateThread)
{
    (void)base;
    const uint32_t handlePtr = ctx.r3.u32, threadIdPtr = ctx.r5.u32;
    auto thread = CreateThread(ctx.r7.u32, ctx.r8.u32, ctx.r6.u32, ctx.r4.u32, (ctx.r9.u32 & kCreateSuspended) != 0);
    if (!thread)
    {
        ctx.r3.u64 = kStatusNoMemory;
        return;
    }
    if (handlePtr)
        memory::Write32(handlePtr, CreateHandle(thread));
    if (threadIdPtr)
        memory::Write32(threadIdPtr, thread->id);
    ctx.r3.u64 = kStatusSuccess;
}

// void ExTerminateThread(uint32_t exitCode)
PPC_FUNC(__imp__ExTerminateThread)
{
    (void)base;
    throw ThreadExit{ ctx.r3.u32 };
}

// NTSTATUS NtResumeThread(uint32_t handle, uint32_t* previousSuspendCount)
PPC_FUNC(__imp__NtResumeThread)
{
    (void)base;
    auto object = FromHandle(ctx.r3.u32);
    if (!object || object->type != ObjectType::Thread)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    const uint32_t previous = ResumeThread(static_cast<Thread&>(*object));
    if (ctx.r4.u32)
        memory::Write32(ctx.r4.u32, previous);
    ctx.r3.u64 = kStatusSuccess;
}

// uint32_t KeResumeThread(KTHREAD* thread)
PPC_FUNC(__imp__KeResumeThread)
{
    (void)base;
    Thread* thread = ThreadFromGuest(ctx.r3.u32);
    ctx.r3.u64 = thread ? ResumeThread(*thread) : 0;
}

// NTSTATUS NtSuspendThread(uint32_t handle, uint32_t* previousSuspendCount)
// Solo surte efecto en hilos que aún no han arrancado: suspender un hilo en marcha no está soportado.
PPC_FUNC(__imp__NtSuspendThread)
{
    (void)base;
    auto object = FromHandle(ctx.r3.u32);
    if (!object || object->type != ObjectType::Thread)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    auto& thread = static_cast<Thread&>(*object);
    uint32_t previous;
    {
        std::lock_guard lock(DispatcherLock());
        previous = thread.suspendCount++;
    }
    std::fprintf(stderr, "[kernel] NtSuspendThread(hilo %u): no se puede suspender un hilo en marcha\n", thread.id);
    if (ctx.r4.u32)
        memory::Write32(ctx.r4.u32, previous);
    ctx.r3.u64 = kStatusSuccess;
}

// int32_t KeSetBasePriorityThread(KTHREAD* thread, int32_t priority): devuelve la anterior.
// La prioridad se guarda pero no se aplica al hilo del anfitrión.
PPC_FUNC(__imp__KeSetBasePriorityThread)
{
    (void)base;
    Thread* thread = ThreadFromGuest(ctx.r3.u32);
    int32_t previous = 0;
    if (thread)
    {
        previous = thread->priority;
        thread->priority = ctx.r4.s32;
    }
    ctx.r3.u64 = uint64_t(int64_t(previous));
}

// int32_t KeQueryBasePriorityThread(KTHREAD* thread)
PPC_FUNC(__imp__KeQueryBasePriorityThread)
{
    (void)base;
    Thread* thread = ThreadFromGuest(ctx.r3.u32);
    ctx.r3.u64 = uint64_t(int64_t(thread ? thread->priority : 0));
}

// uint32_t KeSetAffinityThread(KTHREAD* thread, uint32_t affinity, uint32_t* previous)
// Se guarda pero no se aplica: macOS no permite fijar hilos a núcleos.
PPC_FUNC(__imp__KeSetAffinityThread)
{
    (void)base;
    Thread* thread = ThreadFromGuest(ctx.r3.u32);
    uint32_t previous = 0;
    if (thread)
    {
        previous = thread->affinity;
        thread->affinity = ctx.r4.u32;
    }
    if (ctx.r5.u32)
        memory::Write32(ctx.r5.u32, previous);
    ctx.r3.u64 = kStatusSuccess;
}

// uint32_t KeSetDisableBoostThread(KTHREAD* thread, uint32_t disable)
PPC_FUNC(__imp__KeSetDisableBoostThread)
{
    (void)base;
    ctx.r3.u64 = 0;
}

// NTSTATUS ObReferenceObjectByHandle(uint32_t handle, void* objectType, void** object)
PPC_FUNC(__imp__ObReferenceObjectByHandle)
{
    (void)base;
    auto object = FromHandle(ctx.r3.u32);
    if (!object)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    if (ctx.r5.u32)
        memory::Write32(ctx.r5.u32, object->guest);
    ctx.r3.u64 = kStatusSuccess;
}

// void ObDereferenceObject(void* object): los objetos se liberan con NtClose.
PPC_FUNC(__imp__ObDereferenceObject)
{
    (void)base;
    ctx.r3.u64 = 0;
}

// NTSTATUS ObLookupThreadByThreadId(uint32_t threadId, KTHREAD** thread)
PPC_FUNC(__imp__ObLookupThreadByThreadId)
{
    (void)base;
    auto thread = FindThreadById(ctx.r3.u32);
    if (!thread)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    memory::Write32(ctx.r4.u32, thread->guest);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS ObOpenObjectByPointer(void* object, void* objectType, uint32_t* handle)
PPC_FUNC(__imp__ObOpenObjectByPointer)
{
    (void)base;
    memory::Write32(ctx.r5.u32, CreateHandle(FromGuest(ctx.r3.u32)));
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtDuplicateObject(uint32_t source, uint32_t* target, uint32_t options)
PPC_FUNC(__imp__NtDuplicateObject)
{
    (void)base;
    constexpr uint32_t kDuplicateCloseSource = 0x1;
    auto object = FromHandle(ctx.r3.u32);
    if (!object)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    if (ctx.r4.u32)
        memory::Write32(ctx.r4.u32, CreateHandle(object));
    if (ctx.r5.u32 & kDuplicateCloseSource)
        CloseHandle(ctx.r3.u32);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtClose(uint32_t handle)
PPC_FUNC(__imp__NtClose)
{
    (void)base;
    ctx.r3.u64 = CloseHandle(ctx.r3.u32) ? kStatusSuccess : kStatusInvalidHandle;
}

// NTSTATUS NtYieldExecution()
PPC_FUNC(__imp__NtYieldExecution)
{
    (void)base;
    sched_yield();
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS KeDelayExecutionThread(uint32_t mode, uint32_t alertable, int64_t* interval)
PPC_FUNC(__imp__KeDelayExecutionThread)
{
    (void)base;
    const uint64_t ns = TimeoutFromGuest(ctx.r5.u32);
    if (ns == 0)
        sched_yield();
    else if (ns != kInfinite)
        std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    else
        std::fprintf(stderr, "[kernel] KeDelayExecutionThread con espera infinita\n");
    ctx.r3.u64 = kStatusSuccess;
}
