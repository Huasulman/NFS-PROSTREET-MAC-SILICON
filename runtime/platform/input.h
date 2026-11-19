#pragma once

#include <cstdint>

// Entrada: mando (framework GameController de macOS) y teclado, convertidos a un mando de
// Xbox 360 (XINPUT_GAMEPAD). La ventana actualiza el estado en el hilo principal; el kernel lo
// lee desde los hilos del juego con GetGamepad.
//
// Sin ventana (pruebas), PROSTREET_PRESS programa pulsaciones: "A@30,START@35.5,DOWN@40" pulsa
// cada botón durante 0,2 s en el segundo indicado desde el arranque.
namespace platform
{
    // Botones de XINPUT_GAMEPAD.
    enum : uint16_t
    {
        kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008,
        kStart = 0x0010, kBack = 0x0020, kLeftThumb = 0x0040, kRightThumb = 0x0080,
        kLeftShoulder = 0x0100, kRightShoulder = 0x0200, kGuide = 0x0400,
        kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000,
    };

    struct Gamepad
    {
        uint16_t buttons = 0;
        uint8_t leftTrigger = 0, rightTrigger = 0;
        int16_t thumbLX = 0, thumbLY = 0, thumbRX = 0, thumbRY = 0;
    };

    // Estado del jugador `user` (solo el 0 está conectado). `packet` cambia cada vez que cambia
    // el estado, como XINPUT_STATE.dwPacketNumber.
    bool GetGamepad(uint32_t user, Gamepad& gamepad, uint32_t& packet);

    // Llamadas desde la ventana (hilo principal).
    void PollControllers();
    void SetKey(uint16_t keyCode, bool down);
    void ReleaseAllKeys();

    // --- Teclado configurable (menú Controles) ---------------------------------------------------
    // Cada acción del mando (un botón, un gatillo o una dirección de un stick) tiene hasta dos
    // teclas (códigos virtuales de macOS). Una tecla solo puede estar en una acción. Las
    // asignaciones se guardan en las preferencias de la app.
    constexpr uint16_t kNoKey = 0xFFFF;
    constexpr uint32_t kKeySlots = 2; // principal y alternativa

    struct KeyAction
    {
        const char* id;   // nombre estable, para las preferencias
        const char* name; // lo que se muestra
    };

    uint32_t KeyActionCount();
    const KeyAction& GetKeyAction(uint32_t action);
    uint16_t BoundKey(uint32_t action, uint32_t slot);
    // Asigna `keyCode` (o kNoKey para dejarla vacía). Si otra acción la tenía, se le quita.
    void BindKey(uint32_t action, uint32_t slot, uint16_t keyCode);
    void ResetKeyBindings();
}
