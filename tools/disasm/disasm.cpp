// Desensambla un rango de la imagen descifrada con el mismo motor que XenonRecomp.
//
// Uso: disasm <imagen.bin> <inicio> <fin>
// Imprime una línea por instrucción: dirección, palabra y ensamblador.

#include <cstdio>
#include <cstdlib>
#include <file.h>
#include <disasm.h>

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr, "Uso: %s <imagen.bin> <inicio> <fin>\n", argv[0]);
        return 1;
    }

    constexpr uint32_t kImageBase = 0x82000000;
    const auto image = LoadFile(argv[1]);
    const uint32_t start = std::strtoul(argv[2], nullptr, 0) & ~3u;
    const uint32_t end = std::strtoul(argv[3], nullptr, 0);
    if (image.empty() || start < kImageBase || end > kImageBase + image.size() || start >= end)
    {
        std::fprintf(stderr, "Rango fuera de la imagen\n");
        return 1;
    }

    for (uint32_t address = start; address < end; address += 4)
    {
        const uint8_t* code = image.data() + (address - kImageBase);
        const uint32_t word = (code[0] << 24) | (code[1] << 16) | (code[2] << 8) | code[3];
        ppc_insn insn;
        if (ppc::Disassemble(code, address, insn) && insn.opcode != nullptr)
            std::printf("%08X  %08X  %-10s %s\n", address, word, insn.opcode->name, insn.op_str);
        else
            std::printf("%08X  %08X  .long\n", address, word);
    }
    return 0;
}
