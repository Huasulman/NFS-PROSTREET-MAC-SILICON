// Entrada: mando (GameController) y teclado → mando de Xbox 360. Ver input.h.

#import <Foundation/Foundation.h>
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

        // Qué hace cada acción en el mando.
        enum class Effect : uint8_t
        {
            Button, RightTrigger, LeftTrigger, LeftX, LeftY, RightX, RightY,
        };

        struct ActionInfo
        {
            KeyAction action;
            Effect effect;
            int16_t value;         // botón (Button) o dirección del stick (+1 / -1)
            uint16_t defaults[kKeySlots];
        };

        // Por defecto:
        //   W / S: acelerar (RT) / frenar (LT)   A / D: volante (stick izq.)   flechas: cruceta
        //   Intro / Espacio: A    Esc / Borrar: B    X, Y: X, Y    Q / E: LB / RB
        //   P: START    Tab: BACK    I J K L: stick derecho (cámara)
        const ActionInfo kActions[] = {
            {{"rt", "Acelerar (RT)"}, Effect::RightTrigger, 0, {kKeyW, kNoKey}},
            {{"lt", "Frenar / marcha atrás (LT)"}, Effect::LeftTrigger, 0, {kKeyS, kNoKey}},
            {{"steer_left", "Girar a la izquierda"}, Effect::LeftX, -1, {kKeyA, kNoKey}},
            {{"steer_right", "Girar a la derecha"}, Effect::LeftX, 1, {kKeyD, kNoKey}},
            {{"left_up", "Stick izquierdo arriba"}, Effect::LeftY, 1, {kNoKey, kNoKey}},
            {{"left_down", "Stick izquierdo abajo"}, Effect::LeftY, -1, {kNoKey, kNoKey}},
            {{"a", "A (aceptar)"}, Effect::Button, int16_t(kA), {kKeyReturn, kKeySpace}},
            {{"b", "B (atrás)"}, Effect::Button, int16_t(kB), {kKeyEscape, kKeyDelete}},
            {{"x", "X"}, Effect::Button, int16_t(kX), {kKeyX, kNoKey}},
            {{"y", "Y"}, Effect::Button, int16_t(kY), {kKeyY, kNoKey}},
            {{"lb", "LB"}, Effect::Button, int16_t(kLeftShoulder), {kKeyQ, kNoKey}},
            {{"rb", "RB"}, Effect::Button, int16_t(kRightShoulder), {kKeyE, kNoKey}},
            {{"start", "START"}, Effect::Button, int16_t(kStart), {kKeyP, kNoKey}},
            {{"back", "BACK"}, Effect::Button, int16_t(kBack), {kKeyTab, kNoKey}},
            {{"dpad_up", "Cruceta arriba"}, Effect::Button, int16_t(kDpadUp), {kKeyUp, kNoKey}},
            {{"dpad_down", "Cruceta abajo"}, Effect::Button, int16_t(kDpadDown), {kKeyDown, kNoKey}},
            {{"dpad_left", "Cruceta izquierda"}, Effect::Button, int16_t(kDpadLeft), {kKeyLeft, kNoKey}},
            {{"dpad_right", "Cruceta derecha"}, Effect::Button, int16_t(kDpadRight), {kKeyRight, kNoKey}},
            {{"camera_up", "Cámara arriba (stick der.)"}, Effect::RightY, 1, {kKeyI, kNoKey}},
            {{"camera_down", "Cámara abajo (stick der.)"}, Effect::RightY, -1, {kKeyK, kNoKey}},
            {{"camera_left", "Cámara izquierda (stick der.)"}, Effect::RightX, -1, {kKeyJ, kNoKey}},
            {{"camera_right", "Cámara derecha (stick der.)"}, Effect::RightX, 1, {kKeyL, kNoKey}},
            {{"ls", "Clic del stick izquierdo (LS)"}, Effect::Button, int16_t(kLeftThumb), {kNoKey, kNoKey}},
            {{"rs", "Clic del stick derecho (RS)"}, Effect::Button, int16_t(kRightThumb), {kNoKey, kNoKey}},
        };
        constexpr uint32_t kActionCount = sizeof(kActions) / sizeof(kActions[0]);

        // Teclas asignadas (con gLock). Se cargan de las preferencias la primera vez.
        uint16_t gBindings[kActionCount][kKeySlots];
        bool gBindingsLoaded = false;
        NSString* const kBindingsDefault = @"KeyBindings";

        void LoadBindings()
        {
            if (gBindingsLoaded)
                return;
            gBindingsLoaded = true;
            NSDictionary* saved = [NSUserDefaults.standardUserDefaults dictionaryForKey:kBindingsDefault];
            for (uint32_t a = 0; a < kActionCount; a++)
            {
                NSArray* keys = saved[[NSString stringWithUTF8String:kActions[a].action.id]];
                for (uint32_t slot = 0; slot < kKeySlots; slot++)
                {
                    gBindings[a][slot] = kActions[a].defaults[slot];
                    if ([keys isKindOfClass:[NSArray class]] && slot < keys.count && [keys[slot] isKindOfClass:[NSNumber class]])
                        gBindings[a][slot] = [keys[slot] unsignedShortValue];
                }
            }
        }

        void SaveBindings()
        {
            NSMutableDictionary* saved = [NSMutableDictionary dictionary];
            for (uint32_t a = 0; a < kActionCount; a++)
                saved[[NSString stringWithUTF8String:kActions[a].action.id]] = @[ @(gBindings[a][0]), @(gBindings[a][1]) ];
            [NSUserDefaults.standardUserDefaults setObject:saved forKey:kBindingsDefault];
        }

        // Teclado (con gLock tomado), según las teclas asignadas.
        Gamepad KeyboardGamepad()
        {
            LoadBindings();
            Gamepad pad;
            int leftX = 0, leftY = 0, rightX = 0, rightY = 0;
            for (uint32_t a = 0; a < kActionCount; a++)
            {
                bool down = false;
                for (uint16_t key : gBindings[a])
                    down |= key < sizeof(gKeys) && gKeys[key];
                if (!down)
                    continue;
                const ActionInfo& info = kActions[a];
                switch (info.effect)
                {
                case Effect::Button: pad.buttons |= uint16_t(info.value); break;
                case Effect::RightTrigger: pad.rightTrigger = 255; break;
                case Effect::LeftTrigger: pad.leftTrigger = 255; break;
                case Effect::LeftX: leftX += info.value; break;
                case Effect::LeftY: leftY += info.value; break;
                case Effect::RightX: rightX += info.value; break;
                case Effect::RightY: rightY += info.value; break;
                }
            }
            auto axis = [](int direction) { return int16_t(std::clamp(direction, -1, 1) * 32767); };
            pad.thumbLX = axis(leftX);
            pad.thumbLY = axis(leftY);
            pad.thumbRX = axis(rightX);
            pad.thumbRY = axis(rightY);
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

    uint32_t KeyActionCount()
    {
        return kActionCount;
    }

    const KeyAction& GetKeyAction(uint32_t action)
    {
        return kActions[action].action;
    }

    uint16_t BoundKey(uint32_t action, uint32_t slot)
    {
        std::lock_guard lock(gLock);
        LoadBindings();
        return gBindings[action][slot];
    }

    void BindKey(uint32_t action, uint32_t slot, uint16_t keyCode)
    {
        std::lock_guard lock(gLock);
        LoadBindings();
        if (keyCode != kNoKey)
            for (auto& keys : gBindings)
                for (uint16_t& key : keys)
                    if (key == keyCode)
                        key = kNoKey;
        gBindings[action][slot] = keyCode;
        SaveBindings();
    }

    void ResetKeyBindings()
    {
        std::lock_guard lock(gLock);
        gBindingsLoaded = true;
        for (uint32_t a = 0; a < kActionCount; a++)
            for (uint32_t slot = 0; slot < kKeySlots; slot++)
                gBindings[a][slot] = kActions[a].defaults[slot];
        [NSUserDefaults.standardUserDefaults removeObjectForKey:kBindingsDefault];
    }
}
