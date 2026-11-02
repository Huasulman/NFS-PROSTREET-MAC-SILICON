#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <ppc_config.h>
#include <ppc_context.h>

// Un "slot" de import: una dirección del juego que el cargador de la Xbox rellena al
// arrancar con el thunk de una función o con la dirección de una variable del kernel.
struct ImportSlot
{
    uint32_t address;
    const char* library;
    const char* name;
    bool isVariable;
    uint32_t thunk;
};

// Tabla generada por tools/gen_imports.py (runtime/generated/imports.cpp).
extern const ImportSlot kImportSlots[];
extern const size_t kImportSlotCount;

// Implementación por defecto de los imports que el runtime aún no implementa:
// registra la llamada y devuelve 0 en r3.
void ImportStub(PPCContext& ctx, uint8_t* base, const char* name);

// Argumento número `index` (0 = el noveno) de una llamada: a partir del noveno van en la pila
// del invitado, en ranuras de 8 bytes desde r1 + 0x54 (según Xenia).
uint32_t StackArgument(const PPCContext& ctx, uint32_t index);

// Último import llamado en este hilo, para los mensajes de error.
extern thread_local const char* gLastImport;

// Idioma de la consola (XC_LANGUAGE_*): 1 = inglés, 3 = alemán, 4 = francés, 5 = español,
// 6 = italiano, 11 = polaco, 12 = ruso. Se elige con PROSTREET_LANGUAGE (por defecto español).
uint32_t ConsoleLanguage();

// Termina en el acto una operación asíncrona de Xam (XOVERLAPPED): escribe el resultado y la
// longitud y señala su evento, si tiene.
void CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length = 0);

// Monta una carpeta del Mac como unidad del juego ("save:" -> data/content/...), como hace
// XamContentCreateEx con los paquetes de partidas guardadas.
void MountDevice(const std::string& device, const std::string& hostPath);
void UnmountDevice(const std::string& device);

// Decodificador XMA: crea la tabla de contextos y publica su dirección (antes de arrancar el juego).
void XmaInitialize();

// Notificaciones del sistema (XNotifyGetNext): PostNotification entrega una a todos los
// listeners; ShowSystemUi envía las de una ventana del sistema que se abre y se cierra.
void PostNotification(uint32_t id, uint32_t param);
void ShowSystemUi();
