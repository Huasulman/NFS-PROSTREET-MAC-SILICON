#pragma once

#include <cstdint>

// Ventana del menú Controles (teclas de cada acción del mando). Desde el hilo principal.
namespace platform
{
    void ShowControlsWindow();

    // ¿Es `keyCode` una tecla modificadora asignable (Shift, Control, Option)? Si lo es, `down`
    // dice si está pulsada según los modificadores de un evento flagsChanged.
    bool IsAssignableModifier(uint16_t keyCode, uint64_t modifierFlags, bool& down);
}
