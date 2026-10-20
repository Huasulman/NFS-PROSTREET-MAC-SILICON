#include "imports.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

thread_local const char* gLastImport = nullptr;

void ImportStub(PPCContext& ctx, uint8_t* base, const char* name)
{
    (void)base;
    gLastImport = name;

    // Las primeras llamadas a cada import se muestran con sus argumentos; después solo se cuentan.
    static std::mutex lock;
    static std::unordered_map<std::string, uint32_t> calls;
    std::unique_lock guard(lock);
    const uint32_t count = ++calls[name];
    if (count <= 3)
    {
        std::fprintf(stderr, "[falta] %-36s r3=%08X r4=%08X r5=%08X r6=%08X  lr=%08X%s\n", name,
            ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr), count == 3 ? "  (no se mostrarán más)" : "");
    }

    ctx.r3.u64 = 0;
}
