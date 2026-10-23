// Contenido y almacenamiento (XamContent*, XamShowDeviceSelectorUI, XamEnumerate): las
// partidas guardadas. Solo hay un dispositivo, el disco duro (id 1), y el selector lo elige
// siempre sin preguntar. Cada paquete de contenido es una carpeta data/content/<nombre>/ y su
// descripción (XCONTENT_DATA) se guarda junto a ella en data/content/<nombre>.xcontent.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "imports.h"
#include "objects.h"
#include "../memory.h"

using namespace kernel;
namespace fs = std::filesystem;

namespace
{
    constexpr uint32_t kErrorSuccess = 0;
    constexpr uint32_t kErrorFileNotFound = 0x2;
    constexpr uint32_t kErrorNoMoreFiles = 0x12;
    constexpr uint32_t kErrorAlreadyExists = 0xB7;
    constexpr uint32_t kErrorIoPending = 0x3E5;
    constexpr uint32_t kErrorDeviceNotConnected = 0x48F;

    constexpr uint32_t kHardDrive = 1;
    constexpr uint64_t kDriveSize = 20ull << 30, kDriveFree = 16ull << 30; // un disco de 20 GB
    constexpr uint64_t kXuid = 0xE000000000000001ull;

    // XCONTENT_DATA (0x134 bytes): dispositivo, tipo, nombre visible (UTF-16, 128) en +8,
    // nombre de archivo (char[42]) en +264.
    constexpr uint32_t kContentDataSize = 0x134;
    constexpr uint32_t kContentFileName = 264;

    bool Trace() { return std::getenv("PROSTREET_TRACE") != nullptr; }

    fs::path ContentRoot()
    {
        return fs::path("data") / "content";
    }

    std::string FileName(uint32_t contentData)
    {
        const char* name = static_cast<const char*>(memory::Translate(contentData + kContentFileName));
        return std::string(name, strnlen(name, 42));
    }

    // Devuelve el resultado: con XOVERLAPPED se completa en el acto y se devuelve "pendiente".
    void Finish(PPCContext& ctx, uint32_t overlapped, uint32_t result, uint32_t length = 0)
    {
        if (overlapped)
        {
            CompleteOverlapped(overlapped, result, length);
            ctx.r3.u64 = kErrorIoPending;
        }
        else
        {
            ctx.r3.u64 = result;
        }
    }

    void Write64(uint32_t address, uint64_t value)
    {
        memory::Write32(address, uint32_t(value >> 32));
        memory::Write32(address + 4, uint32_t(value));
    }

    // Un enumerador: una lista de elementos ya serializados (en big-endian) que XamEnumerate
    // va entregando de `perCall` en `perCall`.
    struct Enumerator : Object
    {
        uint32_t itemSize = 0, perCall = 1;
        std::vector<std::vector<uint8_t>> items;
        size_t next = 0;

        Enumerator() : Object(ObjectType::Enumerator) {}
        bool IsSignaled(Thread*) const override { return true; }
    };

    uint32_t CreateEnumerator(std::shared_ptr<Enumerator> enumerator)
    {
        return CreateHandle(Register(enumerator));
    }
}

// uint32_t XamShowDeviceSelectorUI(uint32_t user, uint32_t contentType, uint32_t contentFlags,
//     uint64_t totalRequested, uint32_t* deviceId, XOVERLAPPED* overlapped)
PPC_FUNC(__imp__XamShowDeviceSelectorUI)
{
    (void)base;
    const uint32_t deviceId = ctx.r7.u32, overlapped = ctx.r8.u32;
    if (deviceId)
        memory::Write32(deviceId, kHardDrive);
    ShowSystemUi();
    if (Trace())
        std::fprintf(stderr, "[xam] XamShowDeviceSelectorUI: se elige el disco duro\n");
    Finish(ctx, overlapped, kErrorSuccess);
}

// uint32_t XamContentGetDeviceData(uint32_t deviceId, X_CONTENT_DEVICE_DATA* data)
// X_CONTENT_DEVICE_DATA (0x50): id, tipo (1 = disco duro), total y libre (u64), nombre UTF-16[28].
PPC_FUNC(__imp__XamContentGetDeviceData)
{
    (void)base;
    const uint32_t deviceId = ctx.r3.u32, data = ctx.r4.u32;
    if (deviceId != kHardDrive)
    {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }
    std::memset(memory::Translate(data), 0, 0x50);
    memory::Write32(data + 0, kHardDrive);
    memory::Write32(data + 4, 1);
    Write64(data + 8, kDriveSize);
    Write64(data + 16, kDriveFree);
    const char* name = "Disco duro";
    for (uint32_t i = 0; name[i] && i < 27; i++)
        memory::Write16(data + 24 + i * 2, uint16_t(name[i]));
    ctx.r3.u64 = kErrorSuccess;
}

// uint32_t XamContentGetDeviceState(uint32_t deviceId, XOVERLAPPED* overlapped)
PPC_FUNC(__imp__XamContentGetDeviceState)
{
    (void)base;
    Finish(ctx, ctx.r4.u32, ctx.r3.u32 == kHardDrive ? kErrorSuccess : kErrorDeviceNotConnected);
}

// uint32_t XamContentGetDeviceName(uint32_t deviceId, wchar_t* name, uint32_t length)
PPC_FUNC(__imp__XamContentGetDeviceName)
{
    (void)base;
    if (ctx.r3.u32 != kHardDrive)
    {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }
    const char* name = "Disco duro";
    const uint32_t buffer = ctx.r4.u32, length = ctx.r5.u32;
    uint32_t i = 0;
    for (; name[i] && i + 1 < length; i++)
        memory::Write16(buffer + i * 2, uint16_t(name[i]));
    if (length)
        memory::Write16(buffer + i * 2, 0);
    ctx.r3.u64 = kErrorSuccess;
}

// uint32_t XamContentCreateEnumerator(uint32_t user, uint32_t deviceId, uint32_t contentType,
//     uint32_t contentFlags, uint32_t itemsPerEnumerate, uint32_t* bufferSize, uint32_t* handle)
// Lista los paquetes guardados del tipo pedido (las descripciones .xcontent).
PPC_FUNC(__imp__XamContentCreateEnumerator)
{
    (void)base;
    const uint32_t deviceId = ctx.r4.u32, contentType = ctx.r5.u32, perCall = std::max(1u, ctx.r7.u32);
    const uint32_t bufferSize = ctx.r8.u32, handle = ctx.r9.u32;

    auto enumerator = std::make_shared<Enumerator>();
    enumerator->itemSize = kContentDataSize;
    enumerator->perCall = perCall;
    std::error_code error;
    if (deviceId == 0 || deviceId == kHardDrive)
    {
        for (const auto& entry : fs::directory_iterator(ContentRoot(), error))
        {
            if (entry.path().extension() != ".xcontent")
                continue;
            std::ifstream file(entry.path(), std::ios::binary);
            std::vector<uint8_t> item(kContentDataSize);
            file.read(reinterpret_cast<char*>(item.data()), kContentDataSize);
            const uint32_t type = (uint32_t(item[4]) << 24) | (uint32_t(item[5]) << 16) | (uint32_t(item[6]) << 8) | item[7];
            if (file && (contentType == 0 || type == contentType))
                enumerator->items.push_back(std::move(item));
        }
    }
    if (bufferSize)
        memory::Write32(bufferSize, perCall * kContentDataSize);
    memory::Write32(handle, CreateEnumerator(enumerator));
    if (Trace())
        std::fprintf(stderr, "[xam] XamContentCreateEnumerator(tipo 0x%X): %zu paquetes\n", contentType, enumerator->items.size());
    ctx.r3.u64 = kErrorSuccess;
}

// uint32_t XamUserCreateAchievementEnumerator(uint32_t titleId, uint32_t user, uint64_t xuid,
//     uint32_t flags, uint32_t offset, uint32_t count, uint32_t* bufferSize, uint32_t* handle)
// Sin Xbox Live no hay logros que listar: el enumerador está vacío.
PPC_FUNC(__imp__XamUserCreateAchievementEnumerator)
{
    (void)base;
    const uint32_t count = std::max(1u, ctx.r8.u32), bufferSize = ctx.r9.u32, handle = ctx.r10.u32;
    auto enumerator = std::make_shared<Enumerator>();
    enumerator->itemSize = 0x28; // XACHIEVEMENT_DETAILS
    enumerator->perCall = count;
    if (bufferSize)
        memory::Write32(bufferSize, count * enumerator->itemSize);
    memory::Write32(handle, CreateEnumerator(enumerator));
    ctx.r3.u64 = kErrorSuccess;
}

// uint32_t XamEnumerate(uint32_t handle, uint32_t flags, void* buffer, uint32_t bufferSize,
//     uint32_t* itemsReturned, XOVERLAPPED* overlapped)
PPC_FUNC(__imp__XamEnumerate)
{
    (void)base;
    const uint32_t buffer = ctx.r5.u32, bufferSize = ctx.r6.u32, itemsReturned = ctx.r7.u32, overlapped = ctx.r8.u32;
    auto object = FromHandle(ctx.r3.u32);
    auto* enumerator = object && object->type == ObjectType::Enumerator ? static_cast<Enumerator*>(object.get()) : nullptr;

    uint32_t returned = 0;
    if (enumerator && enumerator->itemSize)
    {
        const uint32_t room = std::min(enumerator->perCall, bufferSize / enumerator->itemSize);
        for (; returned < room && enumerator->next < enumerator->items.size(); returned++, enumerator->next++)
        {
            const auto& item = enumerator->items[enumerator->next];
            std::memcpy(memory::Translate(buffer + returned * enumerator->itemSize), item.data(), item.size());
        }
    }
    if (itemsReturned)
        memory::Write32(itemsReturned, returned);
    Finish(ctx, overlapped, returned ? kErrorSuccess : kErrorNoMoreFiles, returned);
}

// uint32_t XamContentCreateEx(uint32_t user, const char* rootName, XCONTENT_DATA* data,
//     uint32_t flags, uint32_t* disposition, uint32_t* licenseMask, uint32_t cacheSize,
//     uint64_t contentSize, XOVERLAPPED* overlapped)
// flags (bits bajos): 1 crear nuevo, 2 crear siempre, 3 abrir existente, 4 abrir siempre,
// 5 truncar existente. disposition: 1 = creado, 2 = abierto.
PPC_FUNC(__imp__XamContentCreateEx)
{
    (void)base;
    const std::string root = static_cast<const char*>(memory::Translate(ctx.r4.u32));
    const uint32_t data = ctx.r5.u32, flags = ctx.r6.u32 & 0xF;
    const uint32_t disposition = ctx.r7.u32, licenseMask = ctx.r8.u32;
    const uint32_t overlapped = StackArgument(ctx, 0);

    const std::string name = FileName(data);
    const fs::path folder = ContentRoot() / name;
    std::error_code error;
    const bool exists = fs::exists(folder, error);

    uint32_t result = kErrorSuccess, how = exists ? 2 : 1;
    if (name.empty())
        result = kErrorFileNotFound;
    else if (flags == 1 && exists)
        result = kErrorAlreadyExists;
    else if ((flags == 3 || flags == 5) && !exists)
        result = kErrorFileNotFound;
    else
    {
        if (exists && (flags == 2 || flags == 5))
        {
            fs::remove_all(folder, error);
            how = 1;
        }
        fs::create_directories(folder, error);
        std::ofstream description(ContentRoot() / (name + ".xcontent"), std::ios::binary | std::ios::trunc);
        std::vector<uint8_t> item(kContentDataSize);
        std::memcpy(item.data(), memory::Translate(data), kContentDataSize);
        item[0] = 0; item[1] = 0; item[2] = 0; item[3] = kHardDrive; // en el disco duro
        description.write(reinterpret_cast<const char*>(item.data()), kContentDataSize);
        MountDevice(root + ":", folder.string());
        if (disposition)
            memory::Write32(disposition, how);
        if (licenseMask)
            memory::Write32(licenseMask, 0xFFFFFFFF);
    }
    if (Trace())
        std::fprintf(stderr, "[xam] XamContentCreateEx(\"%s:\" -> %s, modo %u): 0x%X\n", root.c_str(),
            folder.string().c_str(), flags, result);
    Finish(ctx, overlapped, result);
}

// uint32_t XamContentClose(const char* rootName, XOVERLAPPED* overlapped)
PPC_FUNC(__imp__XamContentClose)
{
    (void)base;
    UnmountDevice(std::string(static_cast<const char*>(memory::Translate(ctx.r3.u32))) + ":");
    Finish(ctx, ctx.r4.u32, kErrorSuccess);
}

// uint32_t XamContentFlush(const char* rootName, XOVERLAPPED* overlapped): los archivos ya
// están en disco.
PPC_FUNC(__imp__XamContentFlush)
{
    (void)base;
    Finish(ctx, ctx.r4.u32, kErrorSuccess);
}

// uint32_t XamContentDelete(uint32_t user, XCONTENT_DATA* data, XOVERLAPPED* overlapped)
PPC_FUNC(__imp__XamContentDelete)
{
    (void)base;
    const std::string name = FileName(ctx.r4.u32);
    std::error_code error;
    uint32_t result = kErrorFileNotFound;
    if (!name.empty() && fs::exists(ContentRoot() / name, error))
    {
        fs::remove_all(ContentRoot() / name, error);
        fs::remove(ContentRoot() / (name + ".xcontent"), error);
        result = kErrorSuccess;
    }
    Finish(ctx, ctx.r5.u32, result);
}

// uint32_t XamContentGetCreator(uint32_t user, XCONTENT_DATA* data, BOOL* isCreator,
//     uint64_t* xuid, XOVERLAPPED* overlapped): el jugador es el creador de todo.
PPC_FUNC(__imp__XamContentGetCreator)
{
    (void)base;
    if (ctx.r5.u32)
        memory::Write32(ctx.r5.u32, 1);
    if (ctx.r6.u32)
        Write64(ctx.r6.u32, kXuid);
    Finish(ctx, ctx.r7.u32, kErrorSuccess);
}

// uint32_t XamContentGetLicenseMask(uint32_t* mask, XOVERLAPPED* overlapped): juego completo.
PPC_FUNC(__imp__XamContentGetLicenseMask)
{
    (void)base;
    if (ctx.r3.u32)
        memory::Write32(ctx.r3.u32, 0xFFFFFFFF);
    Finish(ctx, ctx.r4.u32, kErrorSuccess);
}

// Miniaturas de los paquetes: no se guardan.
// uint32_t XamContentSetThumbnail(const char* rootName, void* image, uint32_t size, XOVERLAPPED*)
PPC_FUNC(__imp__XamContentSetThumbnail)
{
    (void)base;
    Finish(ctx, ctx.r6.u32, kErrorSuccess);
}

// uint32_t XamContentGetThumbnail(uint32_t user, XCONTENT_DATA*, void* image, uint32_t* size, XOVERLAPPED*)
PPC_FUNC(__imp__XamContentGetThumbnail)
{
    (void)base;
    if (ctx.r6.u32)
        memory::Write32(ctx.r6.u32, 0);
    Finish(ctx, ctx.r7.u32, kErrorFileNotFound);
}
