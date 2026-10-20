#include "xex.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>

#include "../memory.h"

namespace xex
{
    static uint32_t gHeader = 0;
    static std::unordered_map<uint32_t, uint32_t> gFields; // clave -> valor tal como está en la cabecera

    static uint32_t ReadBE32(const std::vector<uint8_t>& data, size_t offset)
    {
        return (data[offset] << 24) | (data[offset + 1] << 16) | (data[offset + 2] << 8) | data[offset + 3];
    }

    void Load(const char* path)
    {
        std::ifstream file(path, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (data.size() < 0x18 || std::memcmp(data.data(), "XEX2", 4) != 0)
        {
            std::fprintf(stderr, "No se pudo leer el XEX %s\n", path);
            std::exit(1);
        }

        // Todo lo anterior a los datos del PE es cabecera (opcionales, seguridad, imports...).
        const uint32_t headerSize = ReadBE32(data, 0x08);
        gHeader = memory::AllocateKernel(headerSize, 0x1000);
        std::memcpy(memory::Translate(gHeader), data.data(), headerSize);

        const uint32_t count = ReadBE32(data, 0x14);
        for (uint32_t i = 0; i < count; i++)
            gFields[ReadBE32(data, 0x18 + i * 8)] = ReadBE32(data, 0x1C + i * 8);
    }

    uint32_t HeaderField(uint32_t key)
    {
        auto it = gFields.find(key);
        if (it == gFields.end())
            return 0;
        const uint32_t kind = key & 0xFF;
        return (kind == 0x00 || kind == 0x01) ? it->second : gHeader + it->second;
    }

    uint32_t HeaderAddress() { return gHeader; }
}
