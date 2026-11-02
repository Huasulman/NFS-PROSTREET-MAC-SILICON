// Usuarios y perfiles (XamUser*): hay un único jugador, con sesión iniciada localmente (sin
// Xbox Live) en el mando 0. Los ajustes del perfil que guarda el juego (opciones, y datos
// propios en los ajustes "title specific") se conservan en data/profile/, uno por archivo.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "imports.h"
#include "objects.h"
#include "../memory.h"

using namespace kernel;

namespace
{
    constexpr uint32_t kErrorSuccess = 0;
    constexpr uint32_t kErrorInsufficientBuffer = 0x7A;
    constexpr uint32_t kErrorIoPending = 0x3E5;
    constexpr uint32_t kErrorNoSuchUser = 0x525;

    constexpr uint64_t kXuid = 0xE000000000000001ull; // XUID "offline" (sin Live)
    constexpr const char* kName = "Jugador";

    bool IsSignedIn(uint32_t user)
    {
        return user == 0;
    }

    void Write64(uint32_t address, uint64_t value)
    {
        memory::Write32(address, uint32_t(value >> 32));
        memory::Write32(address + 4, uint32_t(value));
    }

    // --- Ajustes del perfil ----------------------------------------------------------------
    // El identificador de un ajuste lleva su tipo en los 4 bits altos y su tamaño máximo en
    // los bits 16-27. Tipos (X_USER_DATA): 1 int32, 2 int64, 3 double, 4 texto UTF-16,
    // 5 float, 6 binario, 7 fecha.
    constexpr uint32_t kTypeUnicode = 4, kTypeBinary = 6;

    uint32_t SettingType(uint32_t id)
    {
        return id >> 28;
    }

    std::mutex gProfileLock;

    std::filesystem::path SettingPath(uint32_t id)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "%08X.bin", id);
        return std::filesystem::path("data") / "profile" / name;
    }

    bool LoadSetting(uint32_t id, std::vector<uint8_t>& value)
    {
        std::ifstream file(SettingPath(id), std::ios::binary);
        if (!file)
            return false;
        value.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return true;
    }

    void SaveSetting(uint32_t id, const uint8_t* data, size_t size)
    {
        std::error_code error;
        std::filesystem::create_directories(SettingPath(id).parent_path(), error);
        std::ofstream file(SettingPath(id), std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(data), std::streamsize(size));
    }
}

// XOVERLAPPED: InternalLow = resultado, InternalHigh = longitud, hEvent en +12, error extendido en +24.
void CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length)
{
    memory::Write32(overlapped + 0, result);
    memory::Write32(overlapped + 4, length);
    memory::Write32(overlapped + 24, result);
    if (const uint32_t handle = memory::Read32(overlapped + 12))
    {
        auto object = FromHandle(handle);
        if (object && (object->type == ObjectType::NotificationEvent || object->type == ObjectType::SynchronizationEvent))
        {
            auto* event = static_cast<Event*>(object.get());
            {
                std::lock_guard lock(DispatcherLock());
                event->signaled = true;
                event->WriteSignalState(1);
            }
            NotifyAll();
        }
    }
}

// uint32_t XamUserGetSigninState(uint32_t user): 0 = sin sesión, 1 = local, 2 = Live.
PPC_FUNC(__imp__XamUserGetSigninState)
{
    (void)base;
    ctx.r3.u64 = IsSignedIn(ctx.r3.u32) ? 1 : 0;
}

// uint32_t XamUserGetXUID(uint32_t user, uint32_t type, uint64_t* xuid)
PPC_FUNC(__imp__XamUserGetXUID)
{
    (void)base;
    if (!IsSignedIn(ctx.r3.u32))
    {
        ctx.r3.u64 = kErrorNoSuchUser;
        return;
    }
    Write64(ctx.r5.u32, kXuid);
    ctx.r3.u64 = kErrorSuccess;
}

// uint32_t XamUserGetName(uint32_t user, char* buffer, uint32_t size)
PPC_FUNC(__imp__XamUserGetName)
{
    (void)base;
    if (!IsSignedIn(ctx.r3.u32))
    {
        ctx.r3.u64 = kErrorNoSuchUser;
        return;
    }
    const uint32_t buffer = ctx.r4.u32, size = ctx.r5.u32;
    if (size > 0)
    {
        const size_t length = std::min<size_t>(std::strlen(kName), size - 1);
        std::memcpy(memory::Translate(buffer), kName, length);
        memory::Write8(buffer + uint32_t(length), 0);
    }
    ctx.r3.u64 = kErrorSuccess;
}

// uint32_t XamUserGetSigninInfo(uint32_t user, uint32_t flags, X_USER_SIGNIN_INFO* info)
// X_USER_SIGNIN_INFO (40 bytes): xuid u64, ?, estado (+12), ?, ?, nombre char[16] (+24).
PPC_FUNC(__imp__XamUserGetSigninInfo)
{
    (void)base;
    const uint32_t info = ctx.r5.u32;
    std::memset(memory::Translate(info), 0, 40);
    if (!IsSignedIn(ctx.r3.u32))
    {
        ctx.r3.u64 = kErrorNoSuchUser;
        return;
    }
    Write64(info, kXuid);
    memory::Write32(info + 12, 1);
    std::memcpy(memory::Translate(info + 24), kName, std::strlen(kName));
    ctx.r3.u64 = kErrorSuccess;
}

// uint32_t XamUserCheckPrivilege(uint32_t user, uint32_t privilege, BOOL* result): todo permitido.
PPC_FUNC(__imp__XamUserCheckPrivilege)
{
    (void)base;
    memory::Write32(ctx.r5.u32, 1);
    ctx.r3.u64 = kErrorSuccess;
}

// uint32_t XamUserReadProfileSettings(uint32_t titleId, uint32_t user, uint32_t xuidCount,
//     uint64_t* xuids, uint32_t settingCount, uint32_t* settingIds, uint32_t* bufferSize,
//     X_USER_READ_PROFILE_SETTINGS* buffer, XOVERLAPPED* overlapped)
//
// Resultado: cabecera {número de ajustes, puntero a la tabla}, la tabla de
// X_USER_PROFILE_SETTING (40 bytes: origen, usuario/xuid en +8, id en +16, X_USER_DATA en +24)
// y detrás los datos de los ajustes de texto y binarios. Origen 0 = el ajuste no existe.
PPC_FUNC(__imp__XamUserReadProfileSettings)
{
    (void)base;
    const uint32_t user = ctx.r4.u32, settingCount = ctx.r7.u32, settingIds = ctx.r8.u32;
    const uint32_t bufferSizePtr = ctx.r9.u32, buffer = ctx.r10.u32;
    const uint32_t overlapped = StackArgument(ctx, 0);

    std::lock_guard lock(gProfileLock);
    std::vector<std::vector<uint8_t>> values(settingCount);
    std::vector<bool> found(settingCount);
    uint32_t required = 8 + settingCount * 40;
    for (uint32_t i = 0; i < settingCount; i++)
    {
        const uint32_t id = memory::Read32(settingIds + i * 4);
        found[i] = LoadSetting(id, values[i]);
        if (std::getenv("PROSTREET_TRACE"))
            std::fprintf(stderr, "[xam] ajuste de perfil 0x%08X: %s\n", id, found[i] ? "guardado" : "no existe");
        if (found[i] && (SettingType(id) == kTypeBinary || SettingType(id) == kTypeUnicode))
            required += (uint32_t(values[i].size()) + 3) & ~3u;
    }

    uint32_t result;
    if (!buffer || memory::Read32(bufferSizePtr) < required)
    {
        memory::Write32(bufferSizePtr, required);
        result = kErrorInsufficientBuffer;
    }
    else
    {
        std::memset(memory::Translate(buffer), 0, required);
        memory::Write32(buffer, settingCount);
        memory::Write32(buffer + 4, buffer + 8);
        uint32_t extra = buffer + 8 + settingCount * 40;
        for (uint32_t i = 0; i < settingCount; i++)
        {
            const uint32_t id = memory::Read32(settingIds + i * 4);
            const uint32_t setting = buffer + 8 + i * 40;
            memory::Write32(setting + 0, found[i] ? 1 : 0); // origen: 1 = del usuario
            memory::Write32(setting + 8, user);
            memory::Write32(setting + 16, id);
            memory::Write8(setting + 24, uint8_t(SettingType(id)));
            if (!found[i])
                continue;
            if (SettingType(id) == kTypeBinary || SettingType(id) == kTypeUnicode)
            {
                std::memcpy(memory::Translate(extra), values[i].data(), values[i].size());
                memory::Write32(setting + 32, uint32_t(values[i].size()));
                memory::Write32(setting + 36, extra);
                extra += (uint32_t(values[i].size()) + 3) & ~3u;
            }
            else if (values[i].size() == 8)
            {
                std::memcpy(memory::Translate(setting + 32), values[i].data(), 8); // ya en big-endian
            }
        }
        result = kErrorSuccess;
    }

    if (overlapped)
    {
        CompleteOverlapped(overlapped, result);
        ctx.r3.u64 = kErrorIoPending;
    }
    else
    {
        ctx.r3.u64 = result;
    }
}

// uint32_t XamUserWriteProfileSettings(uint32_t titleId, uint32_t user, uint32_t settingCount,
//     X_USER_PROFILE_SETTING* settings, XOVERLAPPED* overlapped)
PPC_FUNC(__imp__XamUserWriteProfileSettings)
{
    (void)base;
    const uint32_t settingCount = ctx.r5.u32, settings = ctx.r6.u32, overlapped = ctx.r7.u32;
    {
        std::lock_guard lock(gProfileLock);
        for (uint32_t i = 0; i < settingCount; i++)
        {
            const uint32_t setting = settings + i * 40;
            const uint32_t id = memory::Read32(setting + 16);
            const uint32_t type = memory::Read8(setting + 24);
            if (type == kTypeBinary || type == kTypeUnicode)
            {
                const uint32_t size = memory::Read32(setting + 32), data = memory::Read32(setting + 36);
                SaveSetting(id, static_cast<const uint8_t*>(memory::Translate(data)), data ? size : 0);
            }
            else
            {
                SaveSetting(id, static_cast<const uint8_t*>(memory::Translate(setting + 32)), 8);
            }
        }
    }
    if (overlapped)
    {
        CompleteOverlapped(overlapped, kErrorSuccess);
        ctx.r3.u64 = kErrorIoPending;
    }
    else
    {
        ctx.r3.u64 = kErrorSuccess;
    }
}
