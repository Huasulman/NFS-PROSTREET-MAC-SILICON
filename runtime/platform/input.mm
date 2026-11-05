// Entrada: mando (GameController) y teclado → mando de Xbox 360. Ver input.h.

#import <GameController/GameController.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "input.h"

namespace platform
{
    namespace
    {
        std::mutex gLock;
        Gamepad gController;     // último estado del mando físico
        bool gKeys[128] = {};    // teclas pulsadas (códigos virtuales de macOS)
        Gamepad gLast;
        uint32_t gPacket = 0;

        const auto gStart = std::chrono::steady_clock::now();

        // Gatillos en el guion (no son bits de botones).
        constexpr uint16_t kScriptRightTrigger = 1, kScriptLeftTrigger = 2;

        struct ScriptedPress
        {
            uint16_t buttons;
            double time, end; // pulsado en [time, end)
            uint16_t triggers = 0;
        };

        uint16_t ButtonByName(const std::string& name)
        {
            static const struct { const char* name; uint16_t button; } kNames[] = {
                {"UP", kDpadUp}, {"DOWN", kDpadDown}, {"LEFT", kDpadLeft}, {"RIGHT", kDpadRight},
                {"START", kStart}, {"BACK", kBack}, {"LS", kLeftThumb}, {"RS", kRightThumb},
                {"LB", kLeftShoulder}, {"RB", kRightShoulder}, {"GUIDE", kGuide},
                {"A", kA}, {"B", kB}, {"X", kX}, {"Y", kY},
            };
            for (const auto& entry : kNames)
                if (name == entry.name)
                    return entry.button;
            std::fprintf(stderr, "[input] PROSTREET_PRESS: botón desconocido '%s'\n", name.c_str());
            return 0;
        }

        // "A@30,START@35.5,RT@40-60": botón@segundo (pulsado 0,2 s) o botón@desde-hasta (mantenido),
        // separados por comas. RT y LT son los gatillos.
        std::vector<ScriptedPress> ParseScript()
        {
            std::vector<ScriptedPress> presses;
            const char* script = std::getenv("PROSTREET_PRESS");
            if (!script)
                return presses;
            std::string text = script;
            size_t position = 0;
            while (position < text.size())
            {
                size_t end = text.find(',', position);
                if (end == std::string::npos)
                    end = text.size();
                const std::string item = text.substr(position, end - position);
                const size_t at = item.find('@');
                if (at != std::string::npos)
                {
                    const std::string name = item.substr(0, at);
                    char* rest = nullptr;
                    const double time = std::strtod(item.c_str() + at + 1, &rest);
                    const double until = *rest == '-' ? std::strtod(rest + 1, nullptr) : time + 0.2;
                    if (name == "RT" || name == "LT")
                        presses.push_back({0, time, until, name == "RT" ? kScriptRightTrigger : kScriptLeftTrigger});
                    else
                        presses.push_back({ButtonByName(name), time, until});
                }
                position = end + 1;
            }
            return presses;
        }

        const std::vector<ScriptedPress> gScript = ParseScript();

        // Botones (y gatillos, en `triggers`) que el guion tiene pulsados ahora.
        uint16_t ScriptedButtons(uint16_t& triggers)
        {
            triggers = 0;
            if (gScript.empty())
                return 0;
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - gStart).count();
            uint16_t buttons = 0;
            for (const auto& press : gScript)
                if (now >= press.time && now < press.end)
                {
                    buttons |= press.buttons;
                    triggers |= press.triggers;
                }
            return buttons;
        }

        int16_t Axis(float value)
        {
            value = std::fmax(-1.0f, std::fmin(1.0f, value));
            return int16_t(std::lround(value * 32767.0f));
        }

        uint8_t Trigger(float value)
        {
            return uint8_t(std::lround(std::fmax(0.0f, std::fmin(1.0f, value)) * 255.0f));
        }

        // Códigos virtuales de teclas de macOS (Carbon kVK_*).
        enum : uint16_t
        {
            kKeyA = 0, kKeyS = 1, kKeyD = 2, kKeyX = 7, kKeyQ = 12, kKeyW = 13, kKeyE = 14, kKeyY = 16,
            kKeyP = 35, kKeyI = 34, kKeyL = 37, kKeyJ = 38, kKeyK = 40,
            kKeyReturn = 36, kKeyTab = 48, kKeySpace = 49, kKeyDelete = 51, kKeyEscape = 53,
            kKeyLeft = 123, kKeyRight = 124, kKeyDown = 125, kKeyUp = 126,
        };

        // Teclado (con gLock tomado):
        //   flechas: cruceta          W / S: acelerar (RT) / frenar (LT)   A / D: volante (stick izq.)
        //   Intro / Espacio: A        Esc / Borrar: B                      X, Y: X, Y
        //   Q / E: LB / RB            P: START                             Tab: BACK
        //   I J K L: stick derecho (cámara)
        Gamepad KeyboardGamepad()
        {
            Gamepad pad;
            static const struct { uint16_t key; uint16_t button; } kButtons[] = {
                {kKeyUp, kDpadUp}, {kKeyDown, kDpadDown}, {kKeyLeft, kDpadLeft}, {kKeyRight, kDpadRight},
                {kKeyReturn, kA}, {kKeySpace, kA}, {kKeyEscape, kB}, {kKeyDelete, kB}, {kKeyX, kX}, {kKeyY, kY},
                {kKeyQ, kLeftShoulder}, {kKeyE, kRightShoulder}, {kKeyP, kStart}, {kKeyTab, kBack},
            };
            for (const auto& entry : kButtons)
                if (gKeys[entry.key])
                    pad.buttons |= entry.button;
            if (gKeys[kKeyW])
                pad.rightTrigger = 255;
            if (gKeys[kKeyS])
                pad.leftTrigger = 255;
            pad.thumbLX = int16_t((gKeys[kKeyD] ? 32767 : 0) - (gKeys[kKeyA] ? 32767 : 0));
            pad.thumbRX = int16_t((gKeys[kKeyL] ? 32767 : 0) - (gKeys[kKeyJ] ? 32767 : 0));
            pad.thumbRY = int16_t((gKeys[kKeyI] ? 32767 : 0) - (gKeys[kKeyK] ? 32767 : 0));
            return pad;
        }

        int16_t Combine(int16_t a, int16_t b)
        {
            return std::abs(a) >= std::abs(b) ? a : b;
        }
    }

    void PollControllers()
    {
        Gamepad pad;
        for (GCController* controller in [GCController controllers])
        {
            GCExtendedGamepad* gamepad = controller.extendedGamepad;
            if (!gamepad)
                continue;
            const struct { GCControllerButtonInput* input; uint16_t button; } kButtons[] = {
                {gamepad.dpad.up, kDpadUp}, {gamepad.dpad.down, kDpadDown},
                {gamepad.dpad.left, kDpadLeft}, {gamepad.dpad.right, kDpadRight},
                {gamepad.buttonMenu, kStart}, {gamepad.buttonOptions, kBack}, {gamepad.buttonHome, kGuide},
                {gamepad.leftThumbstickButton, kLeftThumb}, {gamepad.rightThumbstickButton, kRightThumb},
                {gamepad.leftShoulder, kLeftShoulder}, {gamepad.rightShoulder, kRightShoulder},
                {gamepad.buttonA, kA}, {gamepad.buttonB, kB}, {gamepad.buttonX, kX}, {gamepad.buttonY, kY},
            };
            for (const auto& entry : kButtons)
                if (entry.input && entry.input.pressed)
                    pad.buttons |= entry.button;
            pad.leftTrigger = Trigger(gamepad.leftTrigger.value);
            pad.rightTrigger = Trigger(gamepad.rightTrigger.value);
            pad.thumbLX = Axis(gamepad.leftThumbstick.xAxis.value);
            pad.thumbLY = Axis(gamepad.leftThumbstick.yAxis.value);
            pad.thumbRX = Axis(gamepad.rightThumbstick.xAxis.value);
            pad.thumbRY = Axis(gamepad.rightThumbstick.yAxis.value);
            break; // solo el primer mando (jugador 0)
        }
        std::lock_guard lock(gLock);
        gController = pad;
    }

    void SetKey(uint16_t keyCode, bool down)
    {
        if (keyCode >= sizeof(gKeys))
            return;
        std::lock_guard lock(gLock);
        gKeys[keyCode] = down;
    }

    void ReleaseAllKeys()
    {
        std::lock_guard lock(gLock);
        std::memset(gKeys, 0, sizeof(gKeys));
    }

    bool GetGamepad(uint32_t user, Gamepad& gamepad, uint32_t& packet)
    {
        if (user != 0)
            return false;
        std::lock_guard lock(gLock);
        const Gamepad keyboard = KeyboardGamepad();
        Gamepad pad;
        uint16_t scriptedTriggers;
        pad.buttons = gController.buttons | keyboard.buttons | ScriptedButtons(scriptedTriggers);
        pad.leftTrigger = std::max<uint8_t>({ gController.leftTrigger, keyboard.leftTrigger, uint8_t(scriptedTriggers & kScriptLeftTrigger ? 255 : 0) });
        pad.rightTrigger = std::max<uint8_t>({ gController.rightTrigger, keyboard.rightTrigger, uint8_t(scriptedTriggers & kScriptRightTrigger ? 255 : 0) });
        pad.thumbLX = Combine(gController.thumbLX, keyboard.thumbLX);
        pad.thumbLY = Combine(gController.thumbLY, keyboard.thumbLY);
        pad.thumbRX = Combine(gController.thumbRX, keyboard.thumbRX);
        pad.thumbRY = Combine(gController.thumbRY, keyboard.thumbRY);

        if (std::memcmp(&pad, &gLast, sizeof(pad)) != 0)
        {
            gLast = pad;
            gPacket++;
        }
        gamepad = pad;
        packet = gPacket;
        return true;
    }
}
