#include "command_processor.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "../kernel/threads.h"
#include "../memory.h"

namespace gpu
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // Registros de la GPU (índices de 32 bits; dirección MMIO = 0x7FC80000 + índice * 4).
        constexpr uint32_t kMmioBase = 0x7FC80000;
        constexpr uint32_t kRegCpRbRptr = 0x01C4;
        constexpr uint32_t kRegCpRbWptr = 0x01C5;
        constexpr uint32_t kRegScratchUmsk = 0x01DC;
        constexpr uint32_t kRegScratchAddr = 0x01DD;
        constexpr uint32_t kRegScratchReg0 = 0x0578;
        constexpr uint32_t kRegisterCount = 0x6000;

        // Códigos de los paquetes PM4 de tipo 3 que se tratan.
        constexpr uint32_t kPm4RegRmw = 0x21;
        constexpr uint32_t kPm4IndirectBufferPfd = 0x37;
        constexpr uint32_t kPm4WaitRegMem = 0x3C;
        constexpr uint32_t kPm4MemWrite = 0x3D;
        constexpr uint32_t kPm4IndirectBuffer = 0x3F;
        constexpr uint32_t kPm4CondWrite = 0x45;
        constexpr uint32_t kPm4Interrupt = 0x54;
        constexpr uint32_t kPm4EventWriteShd = 0x58;
        constexpr uint32_t kPm4EventWriteExt = 0x59;

        constexpr auto kVsyncPeriod = std::chrono::nanoseconds(1000000000 / 60);

        struct State
        {
            std::mutex lock; // protege la configuración
            uint32_t ringGuest = 0;         // dirección del ring en el invitado
            uint32_t ringDwords = 0;        // tamaño en palabras de 32 bits
            uint32_t readPointer = 0;       // en palabras
            uint32_t writeBackGuest = 0;    // dónde se publica el puntero de lectura
            uint32_t interruptCallback = 0;
            uint32_t interruptUserData = 0;
            std::array<uint32_t, kRegisterCount> registers{};

            std::atomic<uint64_t> frames{0}, packets{0}, interrupts{0}, vsyncs{0};
            std::shared_ptr<kernel::Thread> thread;       // procesador de comandos
            std::shared_ptr<kernel::Thread> vsyncThread;  // VSync, independiente del anterior
            std::mutex interruptLock;                     // los manejadores no se ejecutan a la vez
        };

        State gState;

        bool Trace() { return std::getenv("PROSTREET_TRACE") != nullptr; }

        uint32_t ReadRegister(uint32_t index) { return memory::Read32(kMmioBase + index * 4); }

        // Intercambio de bytes de la GPU según los 2 bits bajos de la dirección.
        uint32_t GpuSwap(uint32_t value, uint32_t endian)
        {
            switch (endian)
            {
            case 1: return ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF); // 8 en 16
            case 2: return __builtin_bswap32(value);                                      // 8 en 32
            case 3: return (value << 16) | (value >> 16);                                 // 16 en 32
            default: return value;
            }
        }

        // Escritura de la GPU en memoria: dirección física con el modo de intercambio en los bits bajos.
        void GpuWrite(uint32_t addressWithEndian, uint32_t value)
        {
            const uint32_t guest = PhysicalToGuest(addressWithEndian & ~3u);
            *static_cast<uint32_t*>(memory::Translate(guest)) = GpuSwap(value, addressWithEndian & 3);
        }

        uint32_t GpuRead(uint32_t addressWithEndian)
        {
            const uint32_t guest = PhysicalToGuest(addressWithEndian & ~3u);
            return GpuSwap(*static_cast<const uint32_t*>(memory::Translate(guest)), addressWithEndian & 3);
        }

        void WriteRegister(uint32_t index, uint32_t value)
        {
            if (index >= kRegisterCount)
                return;
            gState.registers[index] = value;
            memory::Write32(kMmioBase + index * 4, value);

            // Registros scratch: si su bit de SCRATCH_UMSK está activo, se copian a memoria.
            // D3D los usa como vallas para saber hasta dónde ha llegado la GPU.
            if (index >= kRegScratchReg0 && index < kRegScratchReg0 + 8)
            {
                const uint32_t n = index - kRegScratchReg0;
                if ((gState.registers[kRegScratchUmsk] >> n) & 1)
                    memory::Write32(PhysicalToGuest(gState.registers[kRegScratchAddr] + n * 4), value);
            }
        }

        // Desplazamiento del número de CPU actual en el KPCR (Xenia: X_KPCR::current_cpu). El
        // manejador de D3D lo lee para saber qué bit de CPU debe confirmar.
        constexpr uint32_t kPcrCurrentCpu = 0x10C;

        // Llama al manejador de interrupción del juego, desde el hilo `thread`, como si se
        // ejecutara en la CPU `cpu`.
        void DispatchInterrupt(kernel::Thread& thread, uint32_t source, uint32_t cpu)
        {
            uint32_t callback, userData;
            {
                std::lock_guard lock(gState.lock);
                callback = gState.interruptCallback;
                userData = gState.interruptUserData;
            }
            if (callback == 0)
                return;
            std::lock_guard lock(gState.interruptLock);
            *static_cast<uint8_t*>(memory::Translate(thread.pcr + kPcrCurrentCpu)) = uint8_t(cpu);
            PPCContext& ctx = *thread.ctx;
            ctx.r3.u64 = source;
            ctx.r4.u64 = userData;
            kernel::CallGuest(thread, callback);
            gState.interrupts++;
        }

        // Compara para WAIT_REG_MEM / COND_WRITE: función en los 3 bits bajos de waitInfo.
        bool Compare(uint32_t function, uint32_t value, uint32_t reference)
        {
            switch (function)
            {
            case 0: return false;
            case 1: return value < reference;
            case 2: return value <= reference;
            case 3: return value == reference;
            case 4: return value != reference;
            case 5: return value >= reference;
            case 6: return value > reference;
            default: return true;
            }
        }

        uint32_t PollValue(uint32_t waitInfo, uint32_t pollAddress)
        {
            return (waitInfo & 0x10) ? GpuRead(pollAddress) : gState.registers[pollAddress & 0x7FFF];
        }

        void ExecuteBuffer(uint32_t guest, uint32_t dwords, int depth);

        // Ejecuta un paquete que empieza en `words[i]`; devuelve cuántas palabras ocupa.
        template <typename Read>
        uint32_t ExecutePacket(Read&& read, uint32_t i, int depth)
        {
            const uint32_t header = read(i);
            gState.packets++;
            switch (header >> 30)
            {
            case 0: // escribir `count` registros seguidos (o el mismo, si el bit 15 está activo)
            {
                const uint32_t baseIndex = header & 0x7FFF, count = ((header >> 16) & 0x3FFF) + 1;
                const bool oneRegister = (header >> 15) & 1;
                for (uint32_t n = 0; n < count; n++)
                    WriteRegister(oneRegister ? baseIndex : baseIndex + n, read(i + 1 + n));
                return 1 + count;
            }
            case 1: // dos registros
                WriteRegister(header & 0x7FF, read(i + 1));
                WriteRegister((header >> 11) & 0x7FF, read(i + 2));
                return 3;
            case 2: // NOP
                return 1;
            default:
                break;
            }

            const uint32_t opcode = (header >> 8) & 0x7F, count = ((header >> 16) & 0x3FFF) + 1;
            auto data = [&](uint32_t n) { return read(i + 1 + n); };
            static const bool kTracePackets = std::getenv("PROSTREET_GPU_TRACE") != nullptr;
            if (kTracePackets)
                std::fprintf(stderr, "[pm4] %*s0x%02X x%-4u %08X %08X %08X %08X\n", depth * 2, "", opcode, count,
                    count > 0 ? data(0) : 0, count > 1 ? data(1) : 0, count > 2 ? data(2) : 0, count > 3 ? data(3) : 0);
            switch (opcode)
            {
            case kPm4IndirectBuffer:
            case kPm4IndirectBufferPfd:
                if (depth < 4)
                    ExecuteBuffer(PhysicalToGuest(data(0)), data(1) & 0xFFFFF, depth + 1);
                break;
            case kPm4MemWrite:
                for (uint32_t n = 1; n < count; n++)
                    GpuWrite(data(0) + (n - 1) * 4, data(n));
                break;
            case kPm4EventWriteShd: // escribe un valor (una valla) al terminar lo anterior
                GpuWrite(data(1), (data(0) >> 31) ? uint32_t(gState.frames) : data(2));
                break;
            case kPm4EventWriteExt: // extensión de la pantalla: (0, 8192, 0, 8192, 0, 1) en 16 bits
            {
                static const uint16_t kExtents[] = { 0, 8192 >> 3, 0, 8192 >> 3, 0, 1 };
                const uint32_t guest = PhysicalToGuest(data(1) & ~3u);
                for (uint32_t n = 0; n < 6; n++)
                    memory::Write16(guest + n * 2, kExtents[n]);
                break;
            }
            case kPm4CondWrite:
                if (Compare(data(0) & 7, PollValue(data(0), data(1)) & data(3), data(2)))
                {
                    if (data(0) & 0x100)
                        GpuWrite(data(4), data(5));
                    else
                        WriteRegister(data(4) & 0x7FFF, data(5));
                }
                break;
            case kPm4WaitRegMem:
            {
                // Espera a que la CPU escriba un valor. Con límite, para no bloquear la GPU para siempre.
                const auto deadline = Clock::now() + std::chrono::seconds(1);
                while (!Compare(data(0) & 7, PollValue(data(0), data(1)) & data(3), data(2)))
                {
                    if (Clock::now() > deadline)
                    {
                        std::fprintf(stderr, "[gpu] WAIT_REG_MEM sin cumplirse tras 1 s (0x%08X), se continúa\n", data(1));
                        if (data(0) & 0x10)
                        {
                            const uint32_t physical = data(1) & 0x1FFFFFFC;
                            for (uint32_t alias : { 0x00000000u, 0xA0000000u, 0xC0000000u, 0xE0000000u })
                                std::fprintf(stderr, "[gpu]   [0x%08X] = 0x%08X\n", alias | physical, memory::Read32(alias | physical));
                        }
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
                break;
            }
            case kPm4RegRmw:
            {
                const uint32_t info = data(0);
                uint32_t andMask = data(1), orMask = data(2);
                if ((info >> 31) & 1) andMask = gState.registers[andMask & 0x1FFF];
                if ((info >> 30) & 1) orMask = gState.registers[orMask & 0x1FFF];
                const uint32_t index = info & 0x1FFF;
                WriteRegister(index, (gState.registers[index] & andMask) | orMask);
                break;
            }
            case kPm4Interrupt: // una interrupción por cada CPU de la máscara
                for (uint32_t cpu = 0; cpu < 6; cpu++)
                    if ((data(0) >> cpu) & 1)
                        DispatchInterrupt(*gState.thread, 1, cpu);
                break;
            case kPacketSwap:
                if (data(0) == kSwapMagic)
                {
                    gState.frames++;
                    if (Trace())
                        std::fprintf(stderr, "[gpu] fotograma %llu presentado (%ux%u, búfer 0x%08X)\n",
                            (unsigned long long)gState.frames, data(2), data(3), data(1));
                }
                break;
            default:
                break; // dibujo, constantes, estado...: aún no se interpretan
            }
            return 1 + count;
        }

        void ExecuteBuffer(uint32_t guest, uint32_t dwords, int depth)
        {
            auto read = [guest](uint32_t n) { return memory::Read32(guest + n * 4); };
            for (uint32_t i = 0; i < dwords;)
                i += ExecutePacket(read, i, depth);
        }

        // Procesa el ring desde el puntero de lectura hasta `writePointer` (con vuelta al principio).
        void ExecuteRing(uint32_t writePointer)
        {
            const uint32_t ring = gState.ringGuest, size = gState.ringDwords;
            auto read = [ring, size](uint32_t n) { return memory::Read32(ring + (n % size) * 4); };
            uint32_t i = gState.readPointer;
            const uint32_t end = writePointer < i ? writePointer + size : writePointer;
            while (i < end)
                i += ExecutePacket(read, i, 0);
            gState.readPointer = i % size;

            memory::Write32(kMmioBase + kRegCpRbRptr * 4, gState.readPointer);
            if (gState.writeBackGuest)
                memory::Write32(gState.writeBackGuest, gState.readPointer);
        }

        // Procesador de comandos: vigila CP_RB_WPTR y ejecuta lo nuevo del ring.
        void GpuThread(PPCContext&, uint8_t*)
        {
            while (true)
            {
                const uint32_t writePointer = ReadRegister(kRegCpRbWptr);
                if (gState.ringDwords && writePointer != gState.readPointer && writePointer < gState.ringDwords)
                    ExecuteRing(writePointer);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }

        // VSync a 60 Hz, en su propio hilo: el procesador de comandos puede estar esperando
        // (WAIT_REG_MEM) justo a algo que hace el manejador de VSync.
        void VsyncThread(PPCContext&, uint8_t*)
        {
            auto next = Clock::now() + kVsyncPeriod;
            auto nextReport = Clock::now() + std::chrono::seconds(1);
            while (true)
            {
                std::this_thread::sleep_until(next);
                gState.vsyncs++;
                DispatchInterrupt(*gState.vsyncThread, 0, 2); // en la CPU 2, como en Xenia
                next += kVsyncPeriod;
                const auto now = Clock::now();
                if (next < now)
                    next = now + kVsyncPeriod;
                if (now >= nextReport)
                {
                    std::fprintf(stderr, "[gpu] %llu fotogramas, %llu paquetes, %llu VSync, %llu interrupciones\n",
                        (unsigned long long)gState.frames, (unsigned long long)gState.packets,
                        (unsigned long long)gState.vsyncs, (unsigned long long)gState.interrupts);
                    nextReport += std::chrono::seconds(1);
                }
            }
        }

        // Registros que la GPU real devuelve con valores fijos (según Xenia, GraphicsSystem::ReadRegister).
        // Aquí los registros son memoria normal, así que basta con escribirlos una vez.
        void WriteFixedRegisters()
        {
            memory::Write32(kMmioBase + 0x0F00 * 4, 0x08100748); // RB_EDRAM_TIMING
            memory::Write32(kMmioBase + 0x0F01 * 4, 0x0000200E); // RB_BC_CONTROL
            memory::Write32(kMmioBase + 0x194C * 4, 0x000002D0); // D1MODE_V_COUNTER (línea 720)
            memory::Write32(kMmioBase + 0x1951 * 4, 0x00000001); // estado de interrupción: VBlank
            memory::Write32(kMmioBase + 0x1961 * 4, 0x050002D0); // D1MODE_VIEWPORT_SIZE (1280 x 720)
        }

        void StartThread()
        {
            WriteFixedRegisters();
            if (!gState.thread)
            {
                gState.thread = kernel::CreateHostThread(GpuThread);
                gState.vsyncThread = kernel::CreateHostThread(VsyncThread);
            }
        }
    }

    void InitializeRingBuffer(uint32_t physical, uint32_t sizeLog2)
    {
        {
            std::lock_guard lock(gState.lock);
            gState.ringGuest = PhysicalToGuest(physical);
            gState.ringDwords = (1u << (sizeLog2 + 3)) / 4; // tamaño en bytes = 2^(sizeLog2 + 3)
            gState.readPointer = 0;
        }
        std::fprintf(stderr, "[gpu] ring buffer en 0x%08X (%u palabras)\n", gState.ringGuest, gState.ringDwords);
        StartThread();
    }

    void EnableReadPointerWriteBack(uint32_t physical, uint32_t blockLog2)
    {
        (void)blockLog2;
        std::lock_guard lock(gState.lock);
        gState.writeBackGuest = PhysicalToGuest(physical);
    }

    void SetInterruptCallback(uint32_t callback, uint32_t userData)
    {
        std::lock_guard lock(gState.lock);
        gState.interruptCallback = callback;
        gState.interruptUserData = userData;
    }
}
