#pragma once

#include <cstdint>

// Salida de sonido (CoreAudio). El juego entrega bloques de 256 muestras a 48 kHz en 5.1 (6
// canales, uno detrás de otro); aquí se mezclan a estéreo y se reproducen. La tarjeta de sonido
// marca el ritmo: el hilo de audio del juego espera a que haya hueco antes de pedir el siguiente.
//
// PROSTREET_NO_AUDIO=1 desactiva la salida (el ritmo pasa a ser un temporizador).
namespace platform
{
    constexpr uint32_t kAudioFrameSamples = 256;
    constexpr uint32_t kAudioChannels = 6;

    // Abre la salida. Devuelve false si no hay dispositivo (o está desactivada).
    bool StartAudio();

    // Espera a que quepa otro bloque en la cola (como mucho ~20 ms, para no bloquear al juego
    // si la salida se para).
    void WaitForAudioSpace();

    // Un bloque del juego: 6 canales x 256 muestras, ya en el orden de bytes del Mac.
    void SubmitAudioFrame(const float* planar);
}
