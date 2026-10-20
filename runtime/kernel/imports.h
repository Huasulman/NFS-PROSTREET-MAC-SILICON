#pragma once

#include <cstddef>
#include <cstdint>

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

// Último import llamado, para los mensajes de error.
extern const char* gLastImport;
