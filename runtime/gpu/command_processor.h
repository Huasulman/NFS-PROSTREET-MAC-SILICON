#pragma once

#include <cstdint>

// Procesador de comandos de la GPU (Xenos), primera versión: no dibuja nada.
//
// El juego escribe paquetes PM4 en el ring buffer y avisa escribiendo el puntero de escritura en
// el registro CP_RB_WPTR, mapeado en 0x7FC80714. Como el código recompilado no intercepta esas
// escrituras, caen en la memoria normal del invitado y el hilo de la GPU las vigila desde ahí.
//
// Se procesan los paquetes con efectos visibles para la CPU (escrituras en memoria, registros
// scratch, esperas, interrupciones, búferes indirectos) y se avanza el puntero de lectura; los
// de dibujo se saltan. También genera la interrupción de VSync a 60 Hz.
namespace gpu
{
    // Direcciones físicas (las que usa la GPU) <-> direcciones del invitado (vista 0xA0000000).
    inline uint32_t PhysicalToGuest(uint32_t physical) { return 0xA0000000 | (physical & 0x1FFFFFFF); }
    inline uint32_t GuestToPhysical(uint32_t guest) { return guest & 0x1FFFFFFF; }

    void InitializeRingBuffer(uint32_t physical, uint32_t sizeLog2);
    void EnableReadPointerWriteBack(uint32_t physical, uint32_t blockLog2);
    void SetInterruptCallback(uint32_t callback, uint32_t userData);

    // Código de operación propio (como en Xenia) del paquete que escribe VdSwap en el ring.
    constexpr uint32_t kPacketSwap = 0x64;
    constexpr uint32_t kSwapMagic = 0x53574150; // 'SWAP'
}
