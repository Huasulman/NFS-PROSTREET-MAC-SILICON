// Runtime mínimo de ProStreet Recomp: carga la imagen del juego en la memoria del invitado,
// rellena la tabla de funciones y los imports, y arranca el hilo principal del juego en el
// punto de entrada del XEX.
//
// Uso: prostreet [imagen] [xex]
//   imagen: por defecto analysis/default_image.bin (generada por tools/xex_dump)
//   xex:    por defecto game/default.xex (para la cabecera)

#include <chrono>
#include <csignal>
#include <execinfo.h>
#include <sys/ucontext.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <pthread.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <ppc_config.h>
#include <ppc_context.h>

#include "kernel/imports.h"
#include "kernel/threads.h"
#include "kernel/xex.h"
#include "platform/window.h"
#include "memory.h"

namespace
{
    constexpr uint32_t kEntryPoint = 0x82A679C8;
    constexpr uint32_t kMainStackSize = 1024 * 1024;

    const char* gImagePath = "analysis/default_image.bin";
    const char* gXexPath = "game/default.xex";

    void LoadImage(uint8_t* base)
    {
        std::ifstream file(gImagePath, std::ios::binary);
        std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (data.size() != PPC_IMAGE_SIZE)
        {
            std::fprintf(stderr, "No se pudo leer %s (%zu bytes, se esperaban %llu). Ejecuta tools/xex_dump.\n",
                gImagePath, data.size(), PPC_IMAGE_SIZE);
            std::exit(1);
        }
        std::memcpy(base + PPC_IMAGE_BASE, data.data(), data.size());
    }

}

// Llamada indirecta a una dirección fuera del código (ver ppc_overrides.h).
void PpcInvalidIndirectCall(PPCContext& ctx, uint8_t*, uint32_t target)
{
    std::fprintf(stderr, "\n*** Llamada indirecta a 0x%08X, fuera del código del juego (lr=0x%08X, hilo %u)\n",
        target, uint32_t(ctx.lr), kernel::CurrentThread() ? kernel::CurrentThread()->id : 0);
    std::_Exit(1);
}

namespace
{
    // Llamada indirecta a una dirección que no es el inicio de ninguna función conocida.
    void MissingFunction(PPCContext& ctx, uint8_t*)
    {
        std::fprintf(stderr, "\n*** Llamada indirecta a una función desconocida (r12/ctr=0x%08X, lr=0x%08X, hilo %u)\n",
            ctx.ctr.u32, uint32_t(ctx.lr), kernel::CurrentThread() ? kernel::CurrentThread()->id : 0);
        std::_Exit(1);
    }

    void FillFunctionTable(uint8_t* base)
    {
        // Primero, todas las entradas del rango de código apuntan a la trampa; luego, las reales.
        for (uint64_t address = PPC_CODE_BASE; address < PPC_CODE_BASE + PPC_CODE_SIZE; address += 4)
            PPC_LOOKUP_FUNC(base, address) = MissingFunction;
        size_t count = 0;
        for (auto* m = PPCFuncMappings; m->guest != 0; m++, count++)
            PPC_LOOKUP_FUNC(base, m->guest) = m->host;
        std::printf("Funciones:  %zu en la tabla de llamadas indirectas\n", count);
    }

    void FillImports()
    {
        size_t variables = 0;
        for (size_t i = 0; i < kImportSlotCount; i++)
        {
            const ImportSlot& slot = kImportSlots[i];
            if (!slot.isVariable)
            {
                memory::Write32(slot.address, slot.thunk);
                continue;
            }

            // Variable exportada por el kernel: de momento, un bloque a cero.
            const uint32_t data = memory::AllocateKernel(0x1000, 0x10);
            memory::Write32(slot.address, data);
            variables++;

            if (std::strcmp(slot.name, "XboxHardwareInfo") == 0)
            {
                memory::Write32(data + 0, 0x20);        // Flags
                *static_cast<uint8_t*>(memory::Translate(data + 4)) = 6; // NumberOfProcessors
            }
            else if (std::strcmp(slot.name, "XboxKrnlVersion") == 0)
            {
                memory::Write16(data + 0, 2);           // Major
                memory::Write16(data + 2, 0);           // Minor
                memory::Write16(data + 4, 6132);        // Build (el XDK con el que se compiló el juego)
            }
            else if (std::strcmp(slot.name, "KeTimeStampBundle") == 0)
            {
                // KeTimeStampBundle: tiempo de interrupción (100 ns desde el arranque), hora del
                // sistema (FILETIME) y milisegundos desde el arranque (GetTickCount la lee). El
                // kernel la actualiza solo; aquí, un hilo cada milisegundo.
                std::thread([data] {
                    const auto start = std::chrono::steady_clock::now();
                    for (;;)
                    {
                        const auto elapsed = std::chrono::steady_clock::now() - start;
                        const uint64_t interrupt = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 100);
                        const uint64_t system = uint64_t(kernel::SystemTime());
                        memory::Write32(data + 0, uint32_t(interrupt >> 32));
                        memory::Write32(data + 4, uint32_t(interrupt));
                        memory::Write32(data + 8, uint32_t(system >> 32));
                        memory::Write32(data + 12, uint32_t(system));
                        memory::Write32(data + 16, uint32_t(interrupt / 10000));
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }).detach();
            }
            else if (std::strcmp(slot.name, "XexExecutableModuleHandle") == 0)
            {
                // El valor es un handle de módulo que apunta a su entrada del cargador.
                memory::Write32(data, memory::AllocateKernel(0x100, 0x10));
            }
        }
        std::printf("Imports:    %zu slots (%zu variables del kernel)\n", kImportSlotCount, variables);
    }

    void OnCrash(int signal, siginfo_t* info, void* context)
    {
        // Escritura en una página vigilada (memoria de texturas): se abre y se reintenta.
        if ((signal == SIGBUS || signal == SIGSEGV) && memory::HandleWriteFault(info->si_addr))
            return;
        const auto* address = static_cast<const uint8_t*>(info->si_addr);
        char message[256];
        int length;
        const kernel::Thread* thread = kernel::CurrentThread();
        const unsigned id = thread ? thread->id : 0;
        const uint32_t guest = memory::Contains(address) ? uint32_t(address - memory::Base()) : 0;
        if (thread && guest >= thread->stackEnd - 0x10000 && guest < thread->stackEnd)
            length = std::snprintf(message, sizeof(message), "\n*** Desbordamiento de la pila del invitado (hilo %u, pila de 0x%X bytes)\n",
                id, thread->stackBase - thread->stackEnd);
        else if (memory::Contains(address))
            length = std::snprintf(message, sizeof(message), "\n*** %s en la dirección del invitado 0x%08X (hilo %u, último import: %s)\n",
                strsignal(signal), guest, id, gLastImport ? gLastImport : "ninguno");
        else
            length = std::snprintf(message, sizeof(message), "\n*** %s en la dirección del anfitrión %p (hilo %u, último import: %s)\n",
                strsignal(signal), info->si_addr, id, gLastImport ? gLastImport : "ninguno");
        write(STDERR_FILENO, message, length);

        // Registros del invitado del hilo que falló (los últimos guardados en su contexto).
        if (thread && thread->ctx)
        {
            const PPCContext& c = *thread->ctx;
            const uint32_t regs[] = { c.r1.u32, c.r3.u32, c.r4.u32, c.r5.u32, c.r11.u32, c.r30.u32, c.r31.u32,
                                      uint32_t(c.lr), c.ctr.u32 };
            static const char* const names[] = { "r1", "r3", "r4", "r5", "r11", "r30", "r31", "lr", "ctr" };
            for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
            {
                length = std::snprintf(message, sizeof(message), "    %-3s = 0x%08X\n", names[i], regs[i]);
                write(STDERR_FILENO, message, length);
            }
        }

        // Dónde estaba el anfitrión: contador de programa, registro de enlace y pila de llamadas.
#if defined(__aarch64__)
        const auto* state = &static_cast<ucontext_t*>(context)->uc_mcontext->__ss;
        void* frames[48];
        frames[0] = reinterpret_cast<void*>(state->__pc);
        frames[1] = reinterpret_cast<void*>(state->__lr);
        int count = 2;
        // Recorrer los punteros de marco (x29) mientras sean razonables.
        auto* frame = reinterpret_cast<void**>(state->__fp);
        while (count < 48 && frame && (reinterpret_cast<uintptr_t>(frame) & 7) == 0 && frame[0] > frame)
        {
            frames[count++] = frame[1];
            frame = static_cast<void**>(frame[0]);
        }
        length = std::snprintf(message, sizeof(message), "    pila del anfitrión (pc, lr, ...):\n");
        write(STDERR_FILENO, message, length);
        backtrace_symbols_fd(frames, count, STDERR_FILENO);
#endif
        _exit(128 + signal);
    }
}

int main(int argc, char** argv)
{
    if (argc > 1)
        gImagePath = argv[1];
    if (argc > 2)
        gXexPath = argv[2];
    setvbuf(stdout, nullptr, _IOLBF, 0);

    struct sigaction action{};
    action.sa_sigaction = OnCrash;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
    sigaction(SIGTRAP, &action, nullptr);

    uint8_t* base = memory::Reserve();
    std::printf("Memoria:    4 GB del invitado en %p\n", static_cast<void*>(base));
    LoadImage(base);
    FillFunctionTable(base);
    FillImports();
    xex::Load(gXexPath);

    // El hilo principal del juego es un hilo más del kernel. El hilo principal del programa
    // queda para la ventana (macOS exige que la interfaz vaya en él); al cerrarla, termina todo.
    // Con PROSTREET_NO_WINDOW=1 no hay ventana y el programa espera al hilo del juego.
    std::printf("Arrancando: punto de entrada 0x%08X\n\n", kEntryPoint);
    XmaInitialize();
    auto mainThread = kernel::CreateThread(kEntryPoint, 0, 0, kMainStackSize, false);

    // PROSTREET_XMA_WATCH: anota las escrituras del juego en los registros del decodificador XMA.
    if (std::getenv("PROSTREET_XMA_WATCH"))
    {
        std::thread([] {
            constexpr uint32_t kStart = 0x7FEA1800, kWords = 0x100;
            uint32_t previous[kWords] = {};
            int reported = 0;
            while (reported < 400)
            {
                for (uint32_t i = 0; i < kWords; i++)
                {
                    const uint32_t value = memory::Read32(kStart + i * 4);
                    if (value != previous[i])
                    {
                        std::fprintf(stderr, "[xma-mmio] 0x%08X = 0x%08X (antes 0x%08X)\n", kStart + i * 4, value, previous[i]);
                        previous[i] = value;
                        reported++;
                    }
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }).detach();
    }

    // PROSTREET_THREADS=N: cada N segundos, la última llamada (lr) de cada hilo del juego, para
    // ver dónde se queda atascado.
    if (const char* every = std::getenv("PROSTREET_THREADS"))
    {
        const int seconds = std::max(1, std::atoi(every));
        std::thread([seconds] {
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(seconds));
                std::fprintf(stderr, "[hilos]");
                for (uint32_t id = 1; id < 64; id++)
                    if (auto thread = kernel::FindThreadById(id); thread && thread->ctx && !thread->exited)
                    {
                        std::fprintf(stderr, " %u:%08X", id, uint32_t(thread->ctx->lr));
                        static const char* stackOf = std::getenv("PROSTREET_THREADS_STACK");
                        if (id == 1 || (stackOf && std::strstr(stackOf, (std::string(",") + std::to_string(id) + ",").c_str())))
                        {
                            // Pila del invitado: r1 apunta a la cadena de marcos; el lr guardado
                            // de cada llamante está en (marco anterior - 8).
                            uint32_t frame = thread->ctx->r1.u32;
                            std::fprintf(stderr, " [");
                            for (int depth = 0; depth < 16 && frame > thread->stackEnd && frame < thread->stackBase; depth++)
                            {
                                const uint32_t previous = memory::Read32(frame);
                                if (previous <= frame || previous >= thread->stackBase)
                                    break;
                                std::fprintf(stderr, " %08X", memory::Read32(previous - 8));
                                frame = previous;
                            }
                            std::fprintf(stderr, " ]");
                        }
                    }
                std::fprintf(stderr, "\n");
            }
        }).detach();
    }
    if (std::getenv("PROSTREET_NO_WINDOW"))
    {
        pthread_join(mainThread->host, nullptr);
        std::printf("\nEl hilo principal terminó (código 0x%08X)\n", mainThread->exitCode);
        return 0;
    }
    platform::RunWindow();
    std::_Exit(0);
}

