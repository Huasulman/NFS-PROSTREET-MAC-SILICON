// Runtime mínimo de ProStreet Recomp: carga la imagen del juego en la memoria del invitado,
// rellena la tabla de funciones y los imports, prepara el hilo principal y llama al punto
// de entrada del XEX.
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
#include "kernel/xex.h"
#include "memory.h"

namespace
{
    constexpr uint32_t kEntryPoint = 0x82A679C8;
    constexpr uint32_t kTlsTemplate = 0x8309CA00; // sección .tls
    constexpr uint32_t kTlsTemplateSize = 0x8;
    constexpr uint32_t kTlsDynamicSlots = 64;
    constexpr uint32_t kGuestStackSize = 1024 * 1024;
    constexpr size_t kHostStackSize = 64 * 1024 * 1024;

    // Desplazamientos del KPCR al que apunta r13 (según Xenia, X_KPCR).
    constexpr uint32_t kPcrTlsPtr = 0x00;
    constexpr uint32_t kPcrSelfPtr = 0x30;
    constexpr uint32_t kPcrStackBase = 0x70;
    constexpr uint32_t kPcrStackEnd = 0x74;
    constexpr uint32_t kPcrCurrentThread = 0x100;

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

    void FillFunctionTable(uint8_t* base)
    {
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

    void* GameThread(void*)
    {
        // Pila propia para el manejador de errores: si se desborda la pila del hilo, el
        // manejador necesita otra para poder ejecutarse.
        stack_t signalStack{};
        signalStack.ss_size = 1024 * 1024;
        signalStack.ss_sp = std::malloc(signalStack.ss_size);
        sigaltstack(&signalStack, nullptr);

        uint8_t* base = memory::Base();
        alignas(64) static PPCContext ctx{};

        // Pila del invitado (crece hacia abajo).
        const uint32_t stackEnd = memory::AllocateKernel(kGuestStackSize, 0x1000);
        const uint32_t stackBase = stackEnd + kGuestStackSize;

        // TLS: slots dinámicos (KeTlsAlloc) seguidos de la copia de la plantilla .tls.
        const uint32_t tls = memory::AllocateKernel(kTlsDynamicSlots * 4 + 0x100, 0x10);
        const uint32_t tlsStatic = tls + kTlsDynamicSlots * 4;
        std::memcpy(memory::Translate(tlsStatic), memory::Translate(kTlsTemplate), kTlsTemplateSize);

        const uint32_t thread = memory::AllocateKernel(0x1000, 0x10); // KTHREAD (vacío por ahora)
        const uint32_t pcr = memory::AllocateKernel(0x1000, 0x1000);
        memory::Write32(pcr + kPcrTlsPtr, tlsStatic);
        memory::Write32(pcr + kPcrSelfPtr, pcr);
        memory::Write32(pcr + kPcrStackBase, stackBase);
        memory::Write32(pcr + kPcrStackEnd, stackEnd);
        memory::Write32(pcr + kPcrCurrentThread, thread);

        ctx.r1.u64 = stackBase - 0x100;
        ctx.r13.u64 = pcr;
        ctx.fpscr.loadFromHost();

        std::printf("Arrancando: punto de entrada 0x%08X (pila 0x%08X, KPCR 0x%08X)\n\n", kEntryPoint, stackBase, pcr);
        (PPC_LOOKUP_FUNC(base, kEntryPoint))(ctx, base);
        std::printf("\nEl punto de entrada terminó (r3 = 0x%08X)\n", ctx.r3.u32);
        return nullptr;
    }

    void OnCrash(int signal, siginfo_t* info, void*)
    {
        const auto* address = static_cast<const uint8_t*>(info->si_addr);
        char message[256];
        int length;
        if (memory::Contains(address))
            length = std::snprintf(message, sizeof(message), "\n*** %s en la dirección del invitado 0x%08X (último import: %s)\n",
                strsignal(signal), uint32_t(address - memory::Base()), gLastImport ? gLastImport : "ninguno");
        else
            length = std::snprintf(message, sizeof(message), "\n*** %s en la dirección del anfitrión %p (último import: %s)\n",
                strsignal(signal), info->si_addr, gLastImport ? gLastImport : "ninguno");
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

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, kHostStackSize);
    pthread_t thread;
    pthread_create(&thread, &attributes, GameThread, nullptr);
    pthread_join(thread, nullptr);
    return 0;
}
