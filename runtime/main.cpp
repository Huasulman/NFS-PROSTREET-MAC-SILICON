// Runtime mínimo de ProStreet Recomp: carga la imagen del juego en la memoria del invitado,
// rellena la tabla de funciones y los imports, y arranca el hilo principal del juego en el
// punto de entrada del XEX.
//
// Uso: prostreet [imagen] [xex]
//   imagen: por defecto analysis/default_image.bin (generada por tools/xex_dump)
//   xex:    por defecto game/default.xex (para la cabecera)

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <pthread.h>
#include <unistd.h>
#include <vector>

#include <ppc_config.h>
#include <ppc_context.h>

#include "kernel/imports.h"
#include "kernel/threads.h"
#include "kernel/xex.h"
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
            else if (std::strcmp(slot.name, "XexExecutableModuleHandle") == 0)
            {
                // El valor es un handle de módulo que apunta a su entrada del cargador.
                memory::Write32(data, memory::AllocateKernel(0x100, 0x10));
            }
        }
        std::printf("Imports:    %zu slots (%zu variables del kernel)\n", kImportSlotCount, variables);
    }

    void OnCrash(int signal, siginfo_t* info, void*)
    {
        const auto* address = static_cast<const uint8_t*>(info->si_addr);
        char message[256];
        int length;
        const kernel::Thread* thread = kernel::CurrentThread();
        const unsigned id = thread ? thread->id : 0;
        if (memory::Contains(address))
            length = std::snprintf(message, sizeof(message), "\n*** %s en la dirección del invitado 0x%08X (hilo %u, último import: %s)\n",
                strsignal(signal), uint32_t(address - memory::Base()), id, gLastImport ? gLastImport : "ninguno");
        else
            length = std::snprintf(message, sizeof(message), "\n*** %s en la dirección del anfitrión %p (hilo %u, último import: %s)\n",
                strsignal(signal), info->si_addr, id, gLastImport ? gLastImport : "ninguno");
        write(STDERR_FILENO, message, length);
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

    // El hilo principal del juego es un hilo más del kernel; el programa termina cuando termina él.
    std::printf("Arrancando: punto de entrada 0x%08X\n\n", kEntryPoint);
    auto mainThread = kernel::CreateThread(kEntryPoint, 0, 0, kMainStackSize, false);
    pthread_join(mainThread->host, nullptr);
    std::printf("\nEl hilo principal terminó (código 0x%08X)\n", mainThread->exitCode);
    return 0;
}
