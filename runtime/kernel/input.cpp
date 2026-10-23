// Mando: XamInput*. El estado sale de platform/input (mando de macOS, teclado o guion de
// pruebas) y se escribe en las estructuras XINPUT_* del juego, en big-endian.

#include <cstdio>
#include <cstdlib>

#include "imports.h"
#include "../memory.h"
#include "../platform/input.h"

namespace
{
    constexpr uint32_t kErrorSuccess = 0;
    constexpr uint32_t kErrorDeviceNotConnected = 0x48F;
    constexpr uint32_t kErrorEmpty = 0x10D2;

    // XUSER_INDEX_ANY (0xFF) equivale al primer jugador.
    uint32_t UserIndex(uint32_t user)
    {
        return (user & 0xFF) == 0xFF ? 0 : user;
    }

    // XINPUT_GAMEPAD: buttons u16, triggers u8 x2, sticks s16 x4 (12 bytes).
    void WriteGamepad(uint32_t address, const platform::Gamepad& pad)
    {
        memory::Write16(address + 0, pad.buttons);
        memory::Write8(address + 2, pad.leftTrigger);
        memory::Write8(address + 3, pad.rightTrigger);
        memory::Write16(address + 4, uint16_t(pad.thumbLX));
        memory::Write16(address + 6, uint16_t(pad.thumbLY));
        memory::Write16(address + 8, uint16_t(pad.thumbRX));
        memory::Write16(address + 10, uint16_t(pad.thumbRY));
    }
}

// uint32_t XamInputGetCapabilities(uint32_t user, uint32_t flags, XINPUT_CAPABILITIES* caps)
// XINPUT_CAPABILITIES: type u8, subType u8, flags u16, gamepad (12), vibration (u16 x2).
PPC_FUNC(__imp__XamInputGetCapabilities)
{
    (void)base;
    const uint32_t user = UserIndex(ctx.r3.u32), caps = ctx.r5.u32;
    platform::Gamepad pad;
    uint32_t packet;
    if (!platform::GetGamepad(user, pad, packet))
    {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }
    memory::Write8(caps + 0, 1);  // XINPUT_DEVTYPE_GAMEPAD
    memory::Write8(caps + 1, 1);  // XINPUT_DEVSUBTYPE_GAMEPAD
    memory::Write16(caps + 2, 0);
    platform::Gamepad all;        // qué controles tiene: todos
    all.buttons = 0xF7FF & ~platform::kGuide;
    all.leftTrigger = all.rightTrigger = 0xFF;
    all.thumbLX = all.thumbLY = all.thumbRX = all.thumbRY = int16_t(0xFFC0);
    WriteGamepad(caps + 4, all);
    memory::Write16(caps + 16, 0xFFFF);
    memory::Write16(caps + 18, 0xFFFF);
    ctx.r3.u64 = kErrorSuccess;
}

// uint32_t XamInputGetState(uint32_t user, uint32_t flags, XINPUT_STATE* state)
// XINPUT_STATE: packetNumber u32, gamepad (12).
PPC_FUNC(__imp__XamInputGetState)
{
    (void)base;
    const uint32_t user = UserIndex(ctx.r3.u32), state = ctx.r5.u32;
    platform::Gamepad pad;
    uint32_t packet;
    if (!platform::GetGamepad(user, pad, packet))
    {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }
    if (state)
    {
        memory::Write32(state, packet);
        WriteGamepad(state + 4, pad);
    }
    ctx.r3.u64 = kErrorSuccess;
}

// uint32_t XamInputSetState(uint32_t user, uint32_t flags, XINPUT_VIBRATION* vibration)
// Vibración: de momento se ignora.
PPC_FUNC(__imp__XamInputSetState)
{
    (void)base;
    platform::Gamepad pad;
    uint32_t packet;
    ctx.r3.u64 = platform::GetGamepad(UserIndex(ctx.r3.u32), pad, packet) ? kErrorSuccess : kErrorDeviceNotConnected;
}

// uint32_t XamInputGetKeystrokeEx(uint32_t* user, uint32_t flags, XINPUT_KEYSTROKE* keystroke)
// Pulsaciones como eventos (para teclados de chat y algunos menús): no se generan.
PPC_FUNC(__imp__XamInputGetKeystrokeEx)
{
    (void)base;
    ctx.r3.u64 = kErrorEmpty;
}
