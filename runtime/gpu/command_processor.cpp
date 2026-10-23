#include "command_processor.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "presenter.h"
#include "renderer.h"
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
        constexpr uint32_t kPm4EventWriteExtents = 0x5A; // extensión en pantalla del último dibujo

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
            uint64_t constantsVersion = 1; // cambia al escribir constantes ALU / fetch / bool / loop

            // Predicated tiling: cada paquete predicado solo se ejecuta si su franja está activa.
            uint64_t binMask = ~0ull, binSelect = ~0ull;

            // Shaders activos (microcódigo y huella), cargados con IM_LOAD / IM_LOAD_IMMEDIATE.
            Shader vertexShader, pixelShader;

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
            if (index >= 0x4000 && index < 0x4920)
                gState.constantsVersion++;

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

        // Base de cada tipo de constante en el archivo de registros (según Xenia).
        uint32_t ConstantBase(uint32_t type)
        {
            switch (type)
            {
            case 0: return 0x4000; // ALU: 512 float4 (256 de vértices + 256 de píxeles)
            case 1: return 0x4800; // fetch: vértices y texturas
            case 2: return 0x4900; // booleanas
            case 3: return 0x4908; // bucles
            default: return 0x2000; // registros de estado
            }
        }

        void LoadShader(uint32_t type, std::vector<uint32_t> code)
        {
            Shader& shader = type == 1 ? gState.pixelShader : gState.vertexShader;
            uint64_t hash = 1469598103934665603ull ^ type; // FNV-1a
            for (uint32_t word : code)
                hash = (hash ^ word) * 1099511628211ull;
            shader.hash = hash;
            shader.code = std::move(code);
        }

        // Guarda cada shader distinto en PROSTREET_SHADERS/<vs|ps>_<hash>.bin (palabras big-endian),
        // para analizarlos con tools/xenos_disasm.py. `words` = nullptr: leerlo de `guest`.
        void CaptureShader(uint32_t type, uint32_t guest, uint32_t dwords, const uint32_t* words)
        {
            static const char* directory = std::getenv("PROSTREET_SHADERS");
            static std::mutex lock;
            static std::unordered_set<uint64_t> seen;
            if (!directory || dwords == 0 || dwords > 0x10000)
                return;
            std::vector<uint32_t> code(dwords);
            for (uint32_t n = 0; n < dwords; n++)
                code[n] = words ? words[n] : memory::Read32(guest + n * 4);

            uint64_t hash = 1469598103934665603ull ^ type; // FNV-1a
            for (uint32_t word : code)
                hash = (hash ^ word) * 1099511628211ull;
            {
                std::lock_guard guard(lock);
                if (!seen.insert(hash).second)
                    return;
            }
            char path[512];
            std::snprintf(path, sizeof(path), "%s/%s_%016llx.bin", directory, type == 1 ? "ps" : "vs", (unsigned long long)hash);
            if (FILE* file = std::fopen(path, "wb"))
            {
                for (uint32_t word : code)
                {
                    const uint32_t big = __builtin_bswap32(word);
                    std::fwrite(&big, 4, 1, file);
                }
                std::fclose(file);
            }
        }

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

            // PROSTREET_GPU_TRACE: todos los paquetes; PROSTREET_GPU_TRACE_FROM=N: solo el fotograma N.
            static const bool kTraceAll = std::getenv("PROSTREET_GPU_TRACE") != nullptr;
            static const uint64_t kTraceFrom = std::getenv("PROSTREET_GPU_TRACE_FROM") ?
                std::strtoull(std::getenv("PROSTREET_GPU_TRACE_FROM"), nullptr, 10) : ~0ull;
            const bool kTracePackets = kTraceAll || gState.frames == kTraceFrom;

            // Paquete predicado (bit 0) de una franja que no está activa: se salta.
            if ((header & 1) && !(gState.binSelect & gState.binMask))
            {
                if (kTracePackets)
                    std::fprintf(stderr, "[pm4] %*s0x%02X saltado (franjas: máscara %llX, selección %llX)\n", depth * 2, "",
                        opcode, (unsigned long long)gState.binMask, (unsigned long long)gState.binSelect);
                return 1 + count;
            }
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
            // Extensión en pantalla de lo dibujado: (0, 8192, 0, 8192, 0, 1) en 16 bits, es decir, toda
            // la pantalla. Con predicated tiling, D3D las lee en la CPU entre franjas para decidir
            // en qué franjas sale cada dibujo (reescribe sus SET_BIN_MASK): así se dibuja en todas.
            case kPm4EventWriteExt:
            case kPm4EventWriteExtents:
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
                    if (Clock::now() - (deadline - std::chrono::seconds(1)) < std::chrono::microseconds(200))
                        std::this_thread::yield();
                    else
                        std::this_thread::sleep_for(std::chrono::microseconds(20));
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
                    PresentFrame(data(1), data(2), data(3));
                    if (Trace())
                        std::fprintf(stderr, "[gpu] fotograma %llu presentado (%ux%u, búfer 0x%08X)\n",
                            (unsigned long long)gState.frames, data(2), data(3), data(1));
                }
                break;
            case 0x27: // IM_LOAD: shader desde memoria (dirección física | tipo, inicio << 16 | tamaño)
            {
                std::vector<uint32_t> words(data(1) & 0xFFFF);
                const uint32_t guest = PhysicalToGuest(data(0) & ~3u);
                for (uint32_t n = 0; n < words.size(); n++)
                    words[n] = memory::Read32(guest + n * 4);
                CaptureShader(data(0) & 3, 0, uint32_t(words.size()), words.data());
                LoadShader(data(0) & 3, std::move(words));
                break;
            }
            case 0x2B: // IM_LOAD_IMMEDIATE: shader dentro del paquete (tipo, inicio << 16 | tamaño, microcódigo)
            {
                std::vector<uint32_t> words(data(1) & 0xFFFF);
                for (uint32_t n = 0; n < words.size(); n++)
                    words[n] = data(2 + n);
                CaptureShader(data(0) & 3, 0, uint32_t(words.size()), words.data());
                LoadShader(data(0) & 3, std::move(words));
                break;
            }
            case 0x2D: // SET_CONSTANT: (tipo << 16 | índice), valores
            {
                const uint32_t index = ConstantBase((data(0) >> 16) & 0xFF) + (data(0) & 0x7FF);
                for (uint32_t n = 1; n < count; n++)
                    WriteRegister(index + n - 1, data(n));
                break;
            }
            case 0x2F: // LOAD_ALU_CONSTANT: dirección física, (tipo << 16 | índice), número de palabras
            {
                const uint32_t guest = PhysicalToGuest(data(0) & 0x3FFFFFFF);
                const uint32_t index = ConstantBase((data(1) >> 16) & 0xFF) + (data(1) & 0x7FF);
                const uint32_t size = data(2) & 0xFFF;
                for (uint32_t n = 0; n < size; n++)
                    WriteRegister(index + n, memory::Read32(guest + n * 4));
                break;
            }
            case 0x55: // SET_CONSTANT2: índice de registro absoluto, valores
            case 0x56: // SET_SHADER_CONSTANTS
                for (uint32_t n = 1; n < count; n++)
                    WriteRegister((data(0) & 0xFFFF) + n - 1, data(n));
                break;
            case 0x60: gState.binMask = (gState.binMask & ~0xFFFFFFFFull) | data(0); break;            // SET_BIN_MASK_LO
            case 0x61: gState.binMask = (gState.binMask & 0xFFFFFFFFull) | (uint64_t(data(0)) << 32); break; // SET_BIN_MASK_HI
            case 0x62: gState.binSelect = (gState.binSelect & ~0xFFFFFFFFull) | data(0); break;        // SET_BIN_SELECT_LO
            case 0x63: gState.binSelect = (gState.binSelect & 0xFFFFFFFFull) | (uint64_t(data(0)) << 32); break; // SET_BIN_SELECT_HI
            case 0x22: // DRAW_INDX: [viz], iniciador, [base de índices, tamaño | endian << 30]
            case 0x36: // DRAW_INDX_2: iniciador, [índices inmediatos]
            {
                const uint32_t drawInitiator = opcode == 0x22 ? data(1) : data(0);
                const uint32_t mode = gState.registers[0x2208] & 7; // RB_MODECONTROL
                if (mode == 6) // copia: es un resolve
                {
                    RendererResolve(gState.registers.data());
                }
                else if (mode == 4 || mode == 5) // 5 = solo profundidad
                {
                    DrawCall draw{};
                    draw.registers = gState.registers.data();
                    draw.vertexShader = &gState.vertexShader;
                    draw.pixelShader = &gState.pixelShader;
                    draw.primitive = drawInitiator & 0x3F;
                    draw.count = drawInitiator >> 16;
                    draw.depthOnly = mode == 5;
                    draw.constantsVersion = gState.constantsVersion;
                    const uint32_t source = (drawInitiator >> 6) & 3; // 0 = índices en memoria, 2 = automáticos
                    if (source == 0 && opcode == 0x22)
                    {
                        draw.indexSize = ((drawInitiator >> 11) & 1) ? 4 : 2;
                        draw.indexBase = data(2);
                        draw.indexEndian = data(3) >> 30;
                    }
                    if (source != 1) // índices inmediatos: aún no
                        RendererDraw(draw);
                }
                static const bool kTraceDraws = std::getenv("PROSTREET_GPU_DRAWS") != nullptr;
                if (kTraceDraws || kTracePackets)
                {
                    const auto& r = gState.registers;
                    const uint32_t initiator = opcode == 0x22 ? data(1) : data(0);
                    std::fprintf(stderr, "[draw] prim=%u src=%u n=%u mode=%u vs=%016llx ps=%016llx vf0=%08X:%08X "
                        "tf0=%08X:%08X:%08X SURFACE=%08X COLOR0=%08X COPY=%08X DEST=%08X BLEND=%08X MASK=%08X WINDOW=%08X MODE=%08X VTE=%08X\n",
                        initiator & 0x3F, (initiator >> 6) & 3, initiator >> 16, r[0x2208] & 7,
                        (unsigned long long)gState.vertexShader.hash, (unsigned long long)gState.pixelShader.hash,
                        r[0x4800], r[0x4801], r[0x4800], r[0x4801], r[0x4802], r[0x2000], r[0x2001], r[0x2318], r[0x2319], r[0x2201], r[0x2104], r[0x2080], r[0x2205], r[0x2206]);
                    if ((initiator & 0x3F) == 13)
                    {
                        // Constantes de fetch: vértices 95 (2 palabras) y texturas 0 y 1 (6 palabras).
                        std::fprintf(stderr, "[fetch] vf95=%08X %08X  tf0=%08X %08X %08X %08X %08X %08X  tf1=%08X %08X %08X %08X %08X %08X\n",
                            r[0x4800 + 190], r[0x4800 + 191], r[0x4800], r[0x4801], r[0x4802], r[0x4803], r[0x4804], r[0x4805],
                            r[0x4806], r[0x4807], r[0x4808], r[0x4809], r[0x480A], r[0x480B]);
                    }
                }
                break;
            }
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
            // Sin trabajo, primero se cede el núcleo (yield) y solo tras 1 ms sin nada se duerme: D3D
            // envía cada fotograma en muchos trozos y dormir tras cada uno retrasa a todo el juego.
            auto lastWork = Clock::now();
            while (true)
            {
                const uint32_t writePointer = ReadRegister(kRegCpRbWptr);
                if (gState.ringDwords && writePointer != gState.readPointer && writePointer < gState.ringDwords)
                {
                    ExecuteRing(writePointer);
                    lastWork = Clock::now();
                    continue;
                }
                if (Clock::now() - lastWork < std::chrono::milliseconds(1))
                    std::this_thread::yield();
                else
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
