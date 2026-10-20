// Vuelca la imagen descifrada y descomprimida de un XEX usando XenonUtils,
// lista sus secciones y busca las funciones de guardado/restauración de
// registros que necesita el TOML de XenonRecomp.
//
// Uso: xex_dump <default.xex> <imagen_salida.bin> [simbolos_salida.tsv]
//
// El archivo de símbolos opcional lista los imports del kernel y de XAM
// (dirección, tamaño, nombre) con los nombres que resuelve XenonUtils.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <file.h>
#include <image.h>

struct Pattern
{
    const char* key;
    std::vector<uint8_t> bytes;
};

// Primeras instrucciones de cada función (big-endian). Se usan 3 instrucciones
// en las de GPR/FPR para no confundirlas con versiones inlined dentro de otras funciones.
static const Pattern kPatterns[] = {
    { "restgprlr_14_address", { 0xE9,0xC1,0xFF,0x68, 0xE9,0xE1,0xFF,0x70, 0xEA,0x01,0xFF,0x78 } },
    { "savegprlr_14_address", { 0xF9,0xC1,0xFF,0x68, 0xF9,0xE1,0xFF,0x70, 0xFA,0x01,0xFF,0x78 } },
    { "restfpr_14_address",   { 0xC9,0xCC,0xFF,0x70, 0xC9,0xEC,0xFF,0x78, 0xCA,0x0C,0xFF,0x80 } },
    { "savefpr_14_address",   { 0xD9,0xCC,0xFF,0x70, 0xD9,0xEC,0xFF,0x78, 0xDA,0x0C,0xFF,0x80 } },
    { "restvmx_14_address",   { 0x39,0x60,0xFE,0xE0, 0x7D,0xCB,0x60,0xCE } },
    { "savevmx_14_address",   { 0x39,0x60,0xFE,0xE0, 0x7D,0xCB,0x61,0xCE } },
    { "restvmx_64_address",   { 0x39,0x60,0xFC,0x00, 0x10,0x0B,0x60,0xCB } },
    { "savevmx_64_address",   { 0x39,0x60,0xFC,0x00, 0x10,0x0B,0x61,0xCB } },
};

int main(int argc, char** argv)
{
    if (argc != 3 && argc != 4)
    {
        std::fprintf(stderr, "Uso: %s <default.xex> <imagen_salida.bin> [simbolos_salida.tsv]\n", argv[0]);
        return 1;
    }

    const auto file = LoadFile(argv[1]);
    if (file.empty())
    {
        std::fprintf(stderr, "No se pudo leer %s\n", argv[1]);
        return 1;
    }

    auto image = Image::ParseImage(file.data(), file.size());
    if (image.data == nullptr || image.size == 0)
    {
        std::fprintf(stderr, "No se pudo cargar la imagen del XEX\n");
        return 1;
    }

    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(image.data.get()), image.size);
    out.close();

    std::printf("Imagen:        %s\n", argv[2]);
    std::printf("Base:          0x%08zX\n", image.base);
    std::printf("Tamaño:        0x%X (%u bytes)\n", image.size, image.size);
    std::printf("Entry point:   0x%08zX\n\n", image.entry_point);

    std::printf("Secciones:\n");
    for (const auto& section : image.sections)
    {
        const char* kind = (section.flags & SectionFlags_Code) ? "código" : "datos";
        std::printf("  %-10s 0x%08zX - 0x%08zX  (0x%08X)  %s\n",
            section.name.c_str(), section.base, section.base + section.size, section.size, kind);
    }

    if (argc == 4)
    {
        std::ofstream symbols(argv[3]);
        size_t count = 0;
        for (const auto& symbol : image.symbols)
        {
            if (symbol.type != Symbol_Function)
                continue;
            symbols << "0x" << std::hex << std::uppercase << symbol.address << '\t'
                    << std::dec << symbol.size << '\t' << symbol.name << '\n';
            count++;
        }
        std::printf("\nSímbolos:     %zu imports en %s\n", count, argv[3]);
    }

    std::printf("\n# Direcciones para el TOML de XenonRecomp\n");
    for (const auto& pattern : kPatterns)
    {
        int found = 0;
        for (const auto& section : image.sections)
        {
            if (!(section.flags & SectionFlags_Code))
                continue;

            const size_t n = pattern.bytes.size();
            for (size_t off = 0; off + n <= section.size; off += 4)
            {
                if (std::memcmp(section.data + off, pattern.bytes.data(), n) == 0)
                {
                    std::printf("%s%s = 0x%08zX\n", found ? "# otra coincidencia: " : "",
                        pattern.key, section.base + off);
                    found++;
                }
            }
        }
        if (!found)
            std::printf("# %s: no encontrado\n", pattern.key);
    }
    return 0;
}
