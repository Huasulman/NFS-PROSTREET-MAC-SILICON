#pragma once

#include <cstdint>

// Cabecera del XEX del juego, copiada en la memoria del invitado para que el kernel pueda
// devolver punteros a sus campos.
namespace xex
{
    // Lee la cabecera de `path` y la copia en memoria del kernel. Termina el programa si falla.
    void Load(const char* path);

    // Valor de un campo de la cabecera opcional: el valor en sí para los campos pequeños
    // (byte bajo de la clave 0x00 o 0x01) o la dirección en el invitado de sus datos. 0 si no existe.
    uint32_t HeaderField(uint32_t key);

    // Dirección en el invitado de la cabecera copiada.
    uint32_t HeaderAddress();
}
