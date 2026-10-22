// Sistema de archivos: traduce las rutas del juego a carpetas del Mac.
//
//   D:\  GAME:\  \Device\Cdrom0\   ->  game/            (solo lectura, como el disco)
//   cualquier otra unidad          ->  data/<unidad>/   (escritura: caché, partidas...)
//
// El juego puede crear sus propias letras de unidad con ObCreateSymbolicLink. Las rutas no
// distinguen mayúsculas, como en la Xbox: si una ruta no existe tal cual, se busca cada
// componente sin distinguirlas. La E/S es síncrona: se completa antes de volver.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "imports.h"
#include "objects.h"
#include "threads.h"
#include "../memory.h"

namespace fs = std::filesystem;
using namespace kernel;

namespace
{
    // --- Códigos NTSTATUS e información de IO_STATUS_BLOCK ---------------------------------
    constexpr uint32_t kStatusSuccess = 0x00000000;
    constexpr uint32_t kStatusNoMoreFiles = 0x80000006;
    constexpr uint32_t kStatusNotImplemented = 0xC0000002;
    constexpr uint32_t kStatusInvalidInfoClass = 0xC0000003;
    constexpr uint32_t kStatusInvalidHandle = 0xC0000008;
    constexpr uint32_t kStatusInvalidParameter = 0xC000000D;
    constexpr uint32_t kStatusNoSuchFile = 0xC000000F;
    constexpr uint32_t kStatusEndOfFile = 0xC0000011;
    constexpr uint32_t kStatusAccessDenied = 0xC0000022;
    constexpr uint32_t kStatusObjectNameNotFound = 0xC0000034;
    constexpr uint32_t kStatusObjectNameCollision = 0xC0000035;
    constexpr uint32_t kStatusObjectPathNotFound = 0xC000003A;
    constexpr uint32_t kStatusNotADirectory = 0xC0000103;
    constexpr uint32_t kStatusFileIsADirectory = 0xC00000BA;

    constexpr uint32_t kFileOpened = 1, kFileCreated = 2, kFileOverwritten = 3, kFileDoesNotExist = 5;

    // Disposición de NtCreateFile.
    constexpr uint32_t kFileSupersede = 0, kFileOpen = 1, kFileCreate = 2, kFileOpenIf = 3,
                       kFileOverwrite = 4, kFileOverwriteIf = 5;
    // Opciones de NtCreateFile.
    constexpr uint32_t kFileDirectoryFile = 0x1, kFileNonDirectoryFile = 0x40;

    constexpr uint32_t kAttributeReadOnly = 0x01, kAttributeDirectory = 0x10, kAttributeNormal = 0x80;

    bool Trace() { return std::getenv("PROSTREET_TRACE") != nullptr; }

    // --- Rutas ------------------------------------------------------------------------------

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
        return text;
    }

    std::mutex gLinksLock;
    std::unordered_map<std::string, std::string> gSymbolicLinks; // en minúsculas, sin "\??\"

    struct Resolved
    {
        fs::path host;
        bool readOnly = false;
    };

    // Busca `name` en `directory` sin distinguir mayúsculas. Si no existe, devuelve el nombre tal cual.
    fs::path MatchComponent(const fs::path& directory, const std::string& name)
    {
        std::error_code error;
        if (fs::exists(directory / name, error))
            return directory / name;
        const std::string wanted = Lower(name);
        for (const auto& entry : fs::directory_iterator(directory, error))
            if (Lower(entry.path().filename().string()) == wanted)
                return entry.path();
        return directory / name;
    }

    // Traduce una ruta del juego ("D:\NFS\ZDIR.BIN") a una ruta del Mac.
    Resolved Resolve(std::string guestPath)
    {
        std::replace(guestPath.begin(), guestPath.end(), '/', '\\');
        if (guestPath.rfind("\\??\\", 0) == 0)
            guestPath = guestPath.substr(4);

        // Separar el dispositivo ("D:" o "\Device\Cdrom0") del resto de la ruta.
        std::string device, rest;
        const size_t colon = guestPath.find(':');
        if (colon != std::string::npos && guestPath.find('\\') > colon)
        {
            device = guestPath.substr(0, colon + 1);
            rest = guestPath.substr(colon + 1);
        }
        else if (guestPath.rfind("\\Device\\", 0) == 0 || Lower(guestPath).rfind("\\device\\", 0) == 0)
        {
            const size_t end = guestPath.find('\\', 8);
            device = guestPath.substr(0, end);
            rest = end == std::string::npos ? "" : guestPath.substr(end);
        }
        else
        {
            device = "game:"; // ruta relativa: como si fuera del disco
            rest = "\\" + guestPath;
        }

        std::string key = Lower(device);
        {
            std::lock_guard lock(gLinksLock);
            for (int i = 0; i < 8; i++) // seguir enlaces encadenados
            {
                auto it = gSymbolicLinks.find(key);
                if (it == gSymbolicLinks.end())
                    break;
                key = Lower(it->second);
            }
        }

        Resolved resolved;
        if (key == "d:" || key == "game:" || key == "\\device\\cdrom0")
        {
            resolved.host = fs::path("game");
            resolved.readOnly = true;
        }
        else
        {
            std::string folder;
            for (char c : key)
                if (std::isalnum(static_cast<unsigned char>(c)))
                    folder += c;
            resolved.host = fs::path("data") / (folder.empty() ? "root" : folder);
        }

        size_t start = 0;
        while (start < rest.size())
        {
            size_t end = rest.find('\\', start);
            if (end == std::string::npos)
                end = rest.size();
            const std::string part = rest.substr(start, end - start);
            if (!part.empty() && part != ".")
                resolved.host = part == ".." ? resolved.host.parent_path() : MatchComponent(resolved.host, part);
            start = end + 1;
        }
        return resolved;
    }

    std::string GuestString(uint32_t ansiString)
    {
        if (ansiString == 0)
            return {};
        const uint16_t length = uint16_t(memory::Read32(ansiString) >> 16);
        const uint32_t buffer = memory::Read32(ansiString + 4);
        return std::string(static_cast<const char*>(memory::Translate(buffer)), length);
    }

    // --- Archivos abiertos -------------------------------------------------------------------

    struct File : Object
    {
        std::mutex lock;
        std::string guestPath;
        fs::path host;
        int fd = -1;
        bool directory = false;
        bool readOnly = false;
        uint64_t position = 0;
        std::vector<fs::directory_entry> entries; // para NtQueryDirectoryFile
        size_t nextEntry = 0;
        bool listed = false;

        File() : Object(ObjectType::File) {}
        ~File() override
        {
            if (fd >= 0)
                close(fd);
        }
        bool IsSignaled(Thread*) const override { return true; } // la E/S es síncrona
    };

    File* AsFile(const std::shared_ptr<Object>& object)
    {
        return object && object->type == ObjectType::File ? static_cast<File*>(object.get()) : nullptr;
    }

    // Ruta completa a partir de OBJECT_ATTRIBUTES: RootDirectory (handle), ObjectName (ANSI_STRING*).
    std::string PathFromAttributes(uint32_t attributes)
    {
        std::string name = GuestString(memory::Read32(attributes + 4));
        const uint32_t root = memory::Read32(attributes + 0);
        if (root != 0)
            if (File* directory = AsFile(FromHandle(root)))
                name = directory->guestPath + "\\" + name;
        return name;
    }

    void WriteIoStatus(uint32_t io, uint32_t status, uint32_t information)
    {
        if (io)
        {
            memory::Write32(io + 0, status);
            memory::Write32(io + 4, information);
        }
    }

    void Write64(uint32_t address, uint64_t value)
    {
        memory::Write32(address, uint32_t(value >> 32));
        memory::Write32(address + 4, uint32_t(value));
    }

    uint64_t Read64(uint32_t address)
    {
        return (uint64_t(memory::Read32(address)) << 32) | memory::Read32(address + 4);
    }

    // Señala el evento de una operación de E/S, si el juego pasó uno.
    void SignalIoEvent(uint32_t eventHandle)
    {
        if (eventHandle == 0)
            return;
        auto object = FromHandle(eventHandle);
        if (!object || (object->type != ObjectType::NotificationEvent && object->type != ObjectType::SynchronizationEvent))
            return;
        {
            std::lock_guard lock(DispatcherLock());
            static_cast<Event&>(*object).signaled = true;
            object->WriteSignalState(1);
        }
        NotifyAll();
    }

    uint64_t FileTime(const struct timespec& time)
    {
        constexpr uint64_t kUnixEpochAsFileTime = 116444736000000000ULL;
        return kUnixEpochAsFileTime + uint64_t(time.tv_sec) * 10000000ULL + uint64_t(time.tv_nsec) / 100;
    }

    uint32_t Attributes(const struct stat& info, bool readOnly)
    {
        uint32_t attributes = S_ISDIR(info.st_mode) ? kAttributeDirectory : 0;
        if (readOnly)
            attributes |= kAttributeReadOnly;
        return attributes ? attributes : kAttributeNormal;
    }

    // FILE_NETWORK_OPEN_INFORMATION (0x38 bytes).
    void WriteNetworkOpenInformation(uint32_t out, const struct stat& info, bool readOnly)
    {
        Write64(out + 0x00, FileTime(info.st_birthtimespec)); // CreationTime
        Write64(out + 0x08, FileTime(info.st_atimespec));     // LastAccessTime
        Write64(out + 0x10, FileTime(info.st_mtimespec));     // LastWriteTime
        Write64(out + 0x18, FileTime(info.st_ctimespec));     // ChangeTime
        Write64(out + 0x20, uint64_t(info.st_blocks) * 512);  // AllocationSize
        Write64(out + 0x28, uint64_t(info.st_size));          // EndOfFile
        memory::Write32(out + 0x30, Attributes(info, readOnly));
        memory::Write32(out + 0x34, 0);
    }

    bool WildcardMatch(const char* pattern, const char* text)
    {
        if (*pattern == '\0')
            return *text == '\0';
        if (*pattern == '*')
            return WildcardMatch(pattern + 1, text) || (*text && WildcardMatch(pattern, text + 1));
        if (*text && (*pattern == '?' || std::tolower(static_cast<unsigned char>(*pattern)) == std::tolower(static_cast<unsigned char>(*text))))
            return WildcardMatch(pattern + 1, text + 1);
        return false;
    }

    // Abre (o crea) un archivo o directorio. Devuelve el NTSTATUS y escribe el handle.
    uint32_t OpenFile(const std::string& guestPath, uint32_t access, uint32_t disposition, uint32_t options,
        uint32_t handlePtr, uint32_t io)
    {
        const Resolved resolved = Resolve(guestPath);
        std::error_code error;
        const bool exists = fs::exists(resolved.host, error);
        const bool isDirectory = exists && fs::is_directory(resolved.host, error);
        const bool wantsWrite = disposition != kFileOpen || (access & 0x40000006) != 0; // GENERIC_WRITE / FILE_WRITE_DATA / FILE_APPEND_DATA
        uint32_t status = kStatusSuccess, information = kFileOpened;

        auto file = std::make_shared<File>();
        file->guestPath = guestPath;
        file->host = resolved.host;
        file->readOnly = resolved.readOnly;

        if (!exists && (disposition == kFileOpen || disposition == kFileOverwrite))
        {
            status = fs::exists(resolved.host.parent_path(), error) ? kStatusObjectNameNotFound : kStatusObjectPathNotFound;
            information = kFileDoesNotExist;
        }
        else if (exists && disposition == kFileCreate)
        {
            status = kStatusObjectNameCollision;
        }
        else if (exists && isDirectory && (options & kFileNonDirectoryFile))
        {
            status = kStatusFileIsADirectory;
        }
        else if (exists && !isDirectory && (options & kFileDirectoryFile))
        {
            status = kStatusNotADirectory;
        }
        else if (resolved.readOnly && wantsWrite && !(exists && disposition == kFileOpenIf))
        {
            status = kStatusAccessDenied;
        }
        else if (isDirectory || (!exists && (options & kFileDirectoryFile)))
        {
            if (!exists)
            {
                fs::create_directories(resolved.host, error);
                information = kFileCreated;
            }
            file->directory = true;
        }
        else
        {
            int flags = resolved.readOnly ? O_RDONLY : O_RDWR;
            if (!exists)
            {
                fs::create_directories(resolved.host.parent_path(), error);
                flags |= O_CREAT;
                information = kFileCreated;
            }
            else if (disposition == kFileSupersede || disposition == kFileOverwrite || disposition == kFileOverwriteIf)
            {
                flags |= O_TRUNC;
                information = kFileOverwritten;
            }
            file->fd = open(resolved.host.c_str(), flags, 0644);
            if (file->fd < 0)
                status = kStatusAccessDenied;
        }

        // Que no exista un archivo es normal (el juego busca primero en su caché, vacía): solo se
        // muestra en modo de diagnóstico.
        if (Trace())
            std::fprintf(stderr, "[archivo] abrir \"%s\" -> %s: %s\n", guestPath.c_str(), resolved.host.c_str(),
                status == kStatusSuccess ? (information == kFileCreated ? "creado" : "ok") : "no se pudo");

        WriteIoStatus(io, status, information);
        memory::Write32(handlePtr, status == kStatusSuccess ? CreateHandle(file) : 0);
        return status;
    }
}

// NTSTATUS NtCreateFile(uint32_t* handle, uint32_t access, OBJECT_ATTRIBUTES* attributes, IO_STATUS_BLOCK* io,
//                       int64_t* allocationSize, uint32_t fileAttributes, uint32_t share, uint32_t disposition,
//                       uint32_t options)   <- noveno argumento: en la pila
PPC_FUNC(__imp__NtCreateFile)
{
    (void)base;
    ctx.r3.u64 = OpenFile(PathFromAttributes(ctx.r5.u32), ctx.r4.u32, ctx.r10.u32, StackArgument(ctx, 0),
        ctx.r3.u32, ctx.r6.u32);
}

// NTSTATUS NtOpenFile(uint32_t* handle, uint32_t access, OBJECT_ATTRIBUTES* attributes,
//                     IO_STATUS_BLOCK* io, uint32_t share, uint32_t options)
PPC_FUNC(__imp__NtOpenFile)
{
    (void)base;
    ctx.r3.u64 = OpenFile(PathFromAttributes(ctx.r5.u32), ctx.r4.u32, kFileOpen, ctx.r8.u32, ctx.r3.u32, ctx.r6.u32);
}

// NTSTATUS NtReadFile(uint32_t handle, uint32_t event, void* apcRoutine, void* apcContext,
//                     IO_STATUS_BLOCK* io, void* buffer, uint32_t length, int64_t* byteOffset)
PPC_FUNC(__imp__NtReadFile)
{
    (void)base;
    File* file = AsFile(FromHandle(ctx.r3.u32));
    const uint32_t io = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, offsetPtr = ctx.r10.u32;
    if (!file || file->fd < 0)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    if (ctx.r5.u32)
        std::fprintf(stderr, "[archivo] NtReadFile con rutina APC: no se ejecutará\n");

    std::lock_guard lock(file->lock);
    uint64_t offset = file->position;
    if (offsetPtr && Read64(offsetPtr) != ~0ull) // -1 = "posición actual"
        offset = Read64(offsetPtr);
    const ssize_t read = pread(file->fd, memory::Translate(buffer), length, off_t(offset));
    const uint32_t status = read < 0 ? kStatusAccessDenied : (read == 0 && length > 0) ? kStatusEndOfFile : kStatusSuccess;
    if (read > 0)
        file->position = offset + uint64_t(read);
    if (Trace())
        std::fprintf(stderr, "[archivo] leer %s @0x%llX: %u de %u bytes\n", file->host.filename().c_str(),
            (unsigned long long)offset, unsigned(std::max<ssize_t>(read, 0)), length);
    WriteIoStatus(io, status, uint32_t(std::max<ssize_t>(read, 0)));
    SignalIoEvent(ctx.r4.u32);
    ctx.r3.u64 = status;
}

// NTSTATUS NtWriteFile(uint32_t handle, uint32_t event, void* apcRoutine, void* apcContext,
//                      IO_STATUS_BLOCK* io, const void* buffer, uint32_t length, int64_t* byteOffset)
PPC_FUNC(__imp__NtWriteFile)
{
    (void)base;
    File* file = AsFile(FromHandle(ctx.r3.u32));
    const uint32_t io = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, offsetPtr = ctx.r10.u32;
    if (!file || file->fd < 0)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    if (file->readOnly)
    {
        ctx.r3.u64 = kStatusAccessDenied;
        return;
    }

    std::lock_guard lock(file->lock);
    uint64_t offset = file->position;
    if (offsetPtr && Read64(offsetPtr) != ~0ull)
        offset = Read64(offsetPtr);
    const ssize_t written = pwrite(file->fd, memory::Translate(buffer), length, off_t(offset));
    const uint32_t status = written < 0 ? kStatusAccessDenied : kStatusSuccess;
    if (written > 0)
        file->position = offset + uint64_t(written);
    WriteIoStatus(io, status, uint32_t(std::max<ssize_t>(written, 0)));
    SignalIoEvent(ctx.r4.u32);
    ctx.r3.u64 = status;
}

// NTSTATUS NtQueryInformationFile(uint32_t handle, IO_STATUS_BLOCK* io, void* info, uint32_t length, uint32_t class)
PPC_FUNC(__imp__NtQueryInformationFile)
{
    (void)base;
    File* file = AsFile(FromHandle(ctx.r3.u32));
    const uint32_t io = ctx.r4.u32, out = ctx.r5.u32, length = ctx.r6.u32, infoClass = ctx.r7.u32;
    if (!file)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    struct stat info{};
    stat(file->host.c_str(), &info);

    uint32_t status = kStatusSuccess, written = 0;
    switch (infoClass)
    {
    case 4: // FileBasicInformation (0x28)
        Write64(out + 0x00, FileTime(info.st_birthtimespec));
        Write64(out + 0x08, FileTime(info.st_atimespec));
        Write64(out + 0x10, FileTime(info.st_mtimespec));
        Write64(out + 0x18, FileTime(info.st_ctimespec));
        memory::Write32(out + 0x20, Attributes(info, file->readOnly));
        memory::Write32(out + 0x24, 0);
        written = 0x28;
        break;
    case 5: // FileStandardInformation (0x18)
        Write64(out + 0x00, uint64_t(info.st_blocks) * 512); // AllocationSize
        Write64(out + 0x08, uint64_t(info.st_size));         // EndOfFile
        memory::Write32(out + 0x10, 1);                      // NumberOfLinks
        *static_cast<uint8_t*>(memory::Translate(out + 0x14)) = 0;              // DeletePending
        *static_cast<uint8_t*>(memory::Translate(out + 0x15)) = file->directory; // Directory
        written = 0x18;
        break;
    case 6: // FileInternalInformation: un identificador único
        Write64(out, uint64_t(info.st_ino));
        written = 8;
        break;
    case 14: // FilePositionInformation
        Write64(out, file->position);
        written = 8;
        break;
    case 34: // FileNetworkOpenInformation
        WriteNetworkOpenInformation(out, info, file->readOnly);
        written = 0x38;
        break;
    case 27: // FileXctdCompressionInformation: no soportada = el archivo no está comprimido con XCTD
        status = kStatusInvalidInfoClass;
        break;
    default:
        std::fprintf(stderr, "[archivo] NtQueryInformationFile: clase %u no implementada\n", infoClass);
        status = kStatusInvalidInfoClass;
        break;
    }
    if (written > length)
        status = kStatusInvalidParameter;
    WriteIoStatus(io, status, status == kStatusSuccess ? written : 0);
    ctx.r3.u64 = status;
}

// NTSTATUS NtSetInformationFile(uint32_t handle, IO_STATUS_BLOCK* io, void* info, uint32_t length, uint32_t class)
PPC_FUNC(__imp__NtSetInformationFile)
{
    (void)base;
    File* file = AsFile(FromHandle(ctx.r3.u32));
    const uint32_t io = ctx.r4.u32, in = ctx.r5.u32, infoClass = ctx.r7.u32;
    if (!file)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    uint32_t status = kStatusSuccess;
    switch (infoClass)
    {
    case 14: // FilePositionInformation
        file->position = Read64(in);
        break;
    case 19: // FileAllocationInformation: el tamaño reservado no importa aquí
        break;
    case 20: // FileEndOfFileInformation
        if (file->readOnly || file->fd < 0 || ftruncate(file->fd, off_t(Read64(in))) != 0)
            status = kStatusAccessDenied;
        break;
    case 13: // FileDispositionInformation: borrar al cerrar
        if (file->readOnly)
            status = kStatusAccessDenied;
        else if (*static_cast<const uint8_t*>(memory::Translate(in)))
        {
            std::error_code error;
            fs::remove(file->host, error);
        }
        break;
    default:
        std::fprintf(stderr, "[archivo] NtSetInformationFile: clase %u no implementada\n", infoClass);
        status = kStatusInvalidInfoClass;
        break;
    }
    WriteIoStatus(io, status, 0);
    ctx.r3.u64 = status;
}

// NTSTATUS NtQueryFullAttributesFile(OBJECT_ATTRIBUTES* attributes, FILE_NETWORK_OPEN_INFORMATION* info)
PPC_FUNC(__imp__NtQueryFullAttributesFile)
{
    (void)base;
    const std::string path = PathFromAttributes(ctx.r3.u32);
    const Resolved resolved = Resolve(path);
    struct stat info{};
    if (stat(resolved.host.c_str(), &info) != 0)
    {
        if (Trace())
            std::fprintf(stderr, "[archivo] atributos \"%s\": no existe\n", path.c_str());
        ctx.r3.u64 = kStatusObjectNameNotFound;
        return;
    }
    WriteNetworkOpenInformation(ctx.r4.u32, info, resolved.readOnly);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtQueryVolumeInformationFile(uint32_t handle, IO_STATUS_BLOCK* io, void* info, uint32_t length, uint32_t class)
PPC_FUNC(__imp__NtQueryVolumeInformationFile)
{
    (void)base;
    File* file = AsFile(FromHandle(ctx.r3.u32));
    const uint32_t io = ctx.r4.u32, out = ctx.r5.u32, length = ctx.r6.u32, infoClass = ctx.r7.u32;
    if (!file)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    uint32_t status = kStatusSuccess, written = 0;
    switch (infoClass)
    {
    case 1: // FileFsVolumeInformation: CreationTime, SerialNumber, LabelLength, SupportsObjects, Label
        Write64(out + 0x00, 0);
        memory::Write32(out + 0x08, 0x12345678);
        memory::Write32(out + 0x0C, 0);
        *static_cast<uint8_t*>(memory::Translate(out + 0x10)) = 0;
        written = 0x14;
        break;
    case 3: // FileFsSizeInformation: TotalAllocationUnits, AvailableAllocationUnits, SectorsPerAllocationUnit, BytesPerSector
    {
        std::error_code error;
        const auto space = fs::space(file->host.has_parent_path() ? file->host.parent_path() : fs::current_path(), error);
        constexpr uint32_t kSectorSize = 0x200, kSectorsPerUnit = 0x20;
        constexpr uint64_t kUnit = uint64_t(kSectorSize) * kSectorsPerUnit;
        Write64(out + 0x00, space.capacity / kUnit);
        Write64(out + 0x08, file->readOnly ? 0 : space.available / kUnit);
        memory::Write32(out + 0x10, kSectorsPerUnit);
        memory::Write32(out + 0x14, kSectorSize);
        written = 0x18;
        break;
    }
    case 5: // FileFsAttributeInformation: FileSystemAttributes, MaximumComponentNameLength, NameLength, Name
    {
        const char* name = file->readOnly ? "GDFX" : "FATX";
        memory::Write32(out + 0x00, 0);
        memory::Write32(out + 0x04, 255);
        memory::Write32(out + 0x08, 4);
        std::memcpy(memory::Translate(out + 0x0C), name, 4);
        written = 0x10;
        break;
    }
    default:
        std::fprintf(stderr, "[archivo] NtQueryVolumeInformationFile: clase %u no implementada\n", infoClass);
        status = kStatusInvalidInfoClass;
        break;
    }
    if (written > length)
        status = kStatusInvalidParameter;
    WriteIoStatus(io, status, status == kStatusSuccess ? written : 0);
    ctx.r3.u64 = status;
}

// NTSTATUS NtQueryDirectoryFile(uint32_t handle, uint32_t event, void* apcRoutine, void* apcContext,
//                               IO_STATUS_BLOCK* io, FILE_DIRECTORY_INFORMATION* info, uint32_t length,
//                               ANSI_STRING* fileMask, uint32_t restartScan)   <- noveno: en la pila
// Devuelve una entrada por llamada.
PPC_FUNC(__imp__NtQueryDirectoryFile)
{
    (void)base;
    File* file = AsFile(FromHandle(ctx.r3.u32));
    const uint32_t io = ctx.r7.u32, out = ctx.r8.u32, length = ctx.r9.u32;
    const std::string mask = GuestString(ctx.r10.u32);
    const bool restart = StackArgument(ctx, 0) != 0;
    if (!file || !file->directory)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }

    std::lock_guard lock(file->lock);
    if (!file->listed || restart)
    {
        file->entries.clear();
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(file->host, error))
            if (mask.empty() || mask == "*" || WildcardMatch(mask.c_str(), entry.path().filename().c_str()))
                file->entries.push_back(entry);
        std::sort(file->entries.begin(), file->entries.end());
        file->nextEntry = 0;
        file->listed = true;
    }

    if (file->nextEntry >= file->entries.size())
    {
        const uint32_t status = file->entries.empty() ? kStatusNoSuchFile : kStatusNoMoreFiles;
        WriteIoStatus(io, status, 0);
        ctx.r3.u64 = status;
        return;
    }

    // FILE_DIRECTORY_INFORMATION: NextEntryOffset, FileIndex, 4 tiempos, EndOfFile,
    // AllocationSize, FileAttributes, FileNameLength, FileName[].
    const auto& entry = file->entries[file->nextEntry];
    const std::string name = entry.path().filename().string();
    if (0x40 + name.size() > length)
    {
        WriteIoStatus(io, kStatusInvalidParameter, 0);
        ctx.r3.u64 = kStatusInvalidParameter;
        return;
    }
    struct stat info{};
    stat(entry.path().c_str(), &info);
    memory::Write32(out + 0x00, 0);
    memory::Write32(out + 0x04, uint32_t(file->nextEntry));
    Write64(out + 0x08, FileTime(info.st_birthtimespec));
    Write64(out + 0x10, FileTime(info.st_atimespec));
    Write64(out + 0x18, FileTime(info.st_mtimespec));
    Write64(out + 0x20, FileTime(info.st_ctimespec));
    Write64(out + 0x28, uint64_t(info.st_size));
    Write64(out + 0x30, uint64_t(info.st_blocks) * 512);
    memory::Write32(out + 0x38, Attributes(info, file->readOnly));
    memory::Write32(out + 0x3C, uint32_t(name.size()));
    std::memcpy(memory::Translate(out + 0x40), name.data(), name.size());
    file->nextEntry++;

    WriteIoStatus(io, kStatusSuccess, uint32_t(0x40 + name.size()));
    SignalIoEvent(ctx.r4.u32);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtFlushBuffersFile(uint32_t handle, IO_STATUS_BLOCK* io)
PPC_FUNC(__imp__NtFlushBuffersFile)
{
    (void)base;
    File* file = AsFile(FromHandle(ctx.r3.u32));
    if (file && file->fd >= 0)
        fsync(file->fd);
    WriteIoStatus(ctx.r4.u32, kStatusSuccess, 0);
    ctx.r3.u64 = file ? kStatusSuccess : kStatusInvalidHandle;
}

// NTSTATUS ObCreateSymbolicLink(ANSI_STRING* link, ANSI_STRING* target)
PPC_FUNC(__imp__ObCreateSymbolicLink)
{
    (void)base;
    std::string link = GuestString(ctx.r3.u32), target = GuestString(ctx.r4.u32);
    if (link.rfind("\\??\\", 0) == 0)
        link = link.substr(4);
    if (Trace())
        std::fprintf(stderr, "[archivo] enlace %s -> %s\n", link.c_str(), target.c_str());
    std::lock_guard lock(gLinksLock);
    gSymbolicLinks[Lower(link)] = target;
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS ObDeleteSymbolicLink(ANSI_STRING* link)
PPC_FUNC(__imp__ObDeleteSymbolicLink)
{
    (void)base;
    std::string link = GuestString(ctx.r3.u32);
    if (link.rfind("\\??\\", 0) == 0)
        link = link.substr(4);
    std::lock_guard lock(gLinksLock);
    gSymbolicLinks.erase(Lower(link));
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtDeviceIoControlFile(uint32_t handle, uint32_t event, void* apcRoutine, void* apcContext,
//                                IO_STATUS_BLOCK* io, uint32_t code, void* input, uint32_t inputLength,
//                                void* output, uint32_t outputLength)
// Sin dispositivos reales: se registra el código y se responde que no está implementado.
PPC_FUNC(__imp__NtDeviceIoControlFile)
{
    (void)base;
    File* file = AsFile(FromHandle(ctx.r3.u32));
    std::fprintf(stderr, "[archivo] NtDeviceIoControlFile 0x%08X sobre \"%s\": no implementado\n", ctx.r8.u32,
        file ? file->guestPath.c_str() : "?");
    WriteIoStatus(ctx.r7.u32, kStatusNotImplemented, 0);
    ctx.r3.u64 = kStatusNotImplemented;
}
