#pragma once
// Se incluye antes que el código generado por XenonRecomp (opción -include en CMakeLists.txt)
// para redefinir algunas de sus macros.

#include <cstdint>

struct PPCContext;

// Llamada indirecta (bctrl) a una dirección fuera del código del juego, p. ej. un puntero a
// función nulo. Implementada en runtime/main.cpp: informa del destino y de quién llamó.
void PpcInvalidIndirectCall(PPCContext& ctx, uint8_t* base, uint32_t target);

// Igual que la de XenonRecomp, pero comprobando que el destino está en el código: si no, la
// búsqueda en la tabla de funciones leería memoria del anfitrión ajena y saltaría a basura.
#define PPC_CALL_INDIRECT_FUNC(x)                                                     \
    do                                                                                \
    {                                                                                 \
        const uint32_t ppcTarget = uint32_t(x);                                       \
        if (ppcTarget - uint32_t(PPC_CODE_BASE) < uint32_t(PPC_CODE_SIZE))            \
            (PPC_LOOKUP_FUNC(base, ppcTarget))(ctx, base);                            \
        else                                                                          \
            PpcInvalidIndirectCall(ctx, base, ppcTarget);                             \
    } while (0)
