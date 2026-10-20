#include "objects.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

#include "threads.h"
#include "../memory.h"

namespace kernel
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        std::mutex gDispatcherLock;
        std::condition_variable gDispatcherSignal;

        std::mutex gTablesLock; // protege las dos tablas siguientes
        std::unordered_map<uint32_t, std::shared_ptr<Object>> gByGuest;
        std::unordered_map<uint32_t, std::shared_ptr<Object>> gByHandle;
        uint32_t gNextHandle = 0xF8000000;

        constexpr uint32_t kHeaderSize = 0x20;
        constexpr uint32_t kSignalStateOffset = 0x04;
        constexpr uint32_t kSemaphoreLimitOffset = 0x10;
    }

    void Object::WriteSignalState(int32_t state) const
    {
        if (guest != 0)
            memory::Write32(guest + kSignalStateOffset, uint32_t(state));
    }

    void Event::Acquire(Thread*)
    {
        if (type == ObjectType::SynchronizationEvent)
        {
            signaled = false;
            WriteSignalState(0);
        }
    }

    void Semaphore::Acquire(Thread*)
    {
        count--;
        WriteSignalState(count);
    }

    void Mutant::Acquire(Thread* waiter)
    {
        owner = waiter;
        recursion++;
        WriteSignalState(0);
    }

    bool Timer::IsSignaled(Thread*) const
    {
        return signaled || Clock::now() >= due;
    }

    void Timer::Acquire(Thread*)
    {
        const auto now = Clock::now();
        if (now >= due)
        {
            if (period.count() > 0)
                while (due <= now)
                    due += period;
            else
                due = Clock::time_point::max();
            signaled = type == ObjectType::NotificationTimer; // el de notificación sigue señalado
        }
        else if (type == ObjectType::SynchronizationTimer)
        {
            signaled = false;
        }
        WriteSignalState(signaled);
    }

    std::mutex& DispatcherLock() { return gDispatcherLock; }

    void NotifyAll() { gDispatcherSignal.notify_all(); }

    std::shared_ptr<Object> Register(std::shared_ptr<Object> object)
    {
        if (object->guest == 0)
            object->guest = memory::AllocateKernel(kHeaderSize, 0x10);
        *static_cast<uint8_t*>(memory::Translate(object->guest)) = uint8_t(object->type);

        std::lock_guard lock(gTablesLock);
        gByGuest[object->guest] = object;
        return object;
    }

    std::shared_ptr<Object> FromGuest(uint32_t header)
    {
        const auto type = ObjectType(*static_cast<const uint8_t*>(memory::Translate(header)));
        const int32_t state = int32_t(memory::Read32(header + kSignalStateOffset));
        {
            std::lock_guard lock(gTablesLock);
            auto it = gByGuest.find(header);
            if (it != gByGuest.end() && it->second->type == type)
                return it->second;
        }

        // Primera vez que se usa (o la memoria se reutilizó para otro tipo): crearlo desde la cabecera.
        std::shared_ptr<Object> object;
        switch (type)
        {
        case ObjectType::NotificationEvent:
        case ObjectType::SynchronizationEvent:
            object = std::make_shared<Event>(type == ObjectType::NotificationEvent, state != 0);
            break;
        case ObjectType::Semaphore:
            object = std::make_shared<Semaphore>(state, int32_t(memory::Read32(header + kSemaphoreLimitOffset)));
            break;
        case ObjectType::Mutant:
            object = std::make_shared<Mutant>();
            break;
        case ObjectType::NotificationTimer:
        case ObjectType::SynchronizationTimer:
            object = std::make_shared<Timer>(type == ObjectType::NotificationTimer);
            break;
        default:
            std::fprintf(stderr, "[kernel] objeto de tipo desconocido %u en 0x%08X; se trata como evento\n", unsigned(type), header);
            object = std::make_shared<Event>(true, state != 0);
            break;
        }
        object->guest = header;
        std::lock_guard lock(gTablesLock);
        gByGuest[header] = object;
        return object;
    }

    uint32_t CreateHandle(std::shared_ptr<Object> object)
    {
        std::lock_guard lock(gTablesLock);
        const uint32_t handle = gNextHandle;
        gNextHandle += 4;
        gByHandle[handle] = std::move(object);
        return handle;
    }

    std::shared_ptr<Object> FromHandle(uint32_t handle)
    {
        if (handle == kCurrentThread)
            return CurrentThreadObject();
        std::lock_guard lock(gTablesLock);
        auto it = gByHandle.find(handle);
        return it == gByHandle.end() ? nullptr : it->second;
    }

    bool CloseHandle(uint32_t handle)
    {
        std::lock_guard lock(gTablesLock);
        return gByHandle.erase(handle) != 0;
    }

    uint64_t TimeoutFromGuest(uint32_t timeoutPtr)
    {
        if (timeoutPtr == 0)
            return kInfinite;
        const int64_t value = int64_t((uint64_t(memory::Read32(timeoutPtr)) << 32) | memory::Read32(timeoutPtr + 4));
        if (value < 0)
            return uint64_t(-value) * 100;
        if (value == 0)
            return 0;
        const int64_t remaining = value - SystemTime(); // tiempo absoluto (FILETIME)
        return remaining > 0 ? uint64_t(remaining) * 100 : 0;
    }

    uint32_t Wait(const std::vector<std::shared_ptr<Object>>& objects, bool waitAll, uint64_t timeoutNs)
    {
        Thread* self = CurrentThread();
        const auto deadline = timeoutNs == kInfinite ? Clock::time_point::max()
                                                     : Clock::now() + std::chrono::nanoseconds(timeoutNs);

        std::unique_lock lock(gDispatcherLock);
        while (true)
        {
            if (waitAll)
            {
                if (std::all_of(objects.begin(), objects.end(), [&](auto& o) { return o->IsSignaled(self); }))
                {
                    for (auto& o : objects)
                        o->Acquire(self);
                    return 0;
                }
            }
            else
            {
                for (size_t i = 0; i < objects.size(); i++)
                {
                    if (objects[i]->IsSignaled(self))
                    {
                        objects[i]->Acquire(self);
                        return uint32_t(i);
                    }
                }
            }

            const auto now = Clock::now();
            if (now >= deadline)
                return kStatusTimeout;

            // Despertar con la próxima señal, el próximo temporizador o el plazo, lo que llegue antes.
            auto wake = deadline;
            for (auto& o : objects)
                wake = std::min(wake, o->NextChange());
            if (wake == Clock::time_point::max())
                gDispatcherSignal.wait(lock);
            else
                gDispatcherSignal.wait_until(lock, wake);
        }
    }
}
