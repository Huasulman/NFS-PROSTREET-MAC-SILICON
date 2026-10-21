#pragma once

namespace platform
{
    // Abre la ventana y ejecuta el bucle de eventos de macOS en el hilo principal. Vuelve al
    // cerrarla. El juego corre en sus propios hilos mientras tanto.
    void RunWindow();
}
