// Traduce los shaders capturados (PROSTREET_SHADERS) y los compila con Metal.
// Uso: test_shader_translator analysis/shaders/*.bin   (con -v muestra el MSL generado)
#import <Metal/Metal.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "gpu/shader_translator.h"

int main(int argc, char** argv)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    bool verbose = false;
    int failures = 0, total = 0;
    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "-v") == 0) { verbose = true; continue; }
        std::ifstream file(argv[i], std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::vector<uint32_t> code(bytes.size() / 4);
        for (size_t n = 0; n < code.size(); n++)
            code[n] = (uint8_t(bytes[n * 4]) << 24) | (uint8_t(bytes[n * 4 + 1]) << 16) | (uint8_t(bytes[n * 4 + 2]) << 8) | uint8_t(bytes[n * 4 + 3]);
        const bool pixel = std::strstr(argv[i], "/ps_") != nullptr;
        auto shader = gpu::TranslateShader(pixel, code, 0x1234 + i);
        if (verbose)
            std::printf("%s\n", shader.source.substr(shader.source.find(pixel ? "fragment" : "vertex VSOut")).c_str());
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:shader.source.c_str()] options:nil error:&error];
        id<MTLFunction> function = [library newFunctionWithName:[NSString stringWithUTF8String:shader.entry.c_str()]];
        total++;
        if (!function)
        {
            failures++;
            std::printf("FALLO %s: %s\n", argv[i], error ? error.localizedDescription.UTF8String : "sin función");
        }
        else
        {
            std::printf("ok    %s%s%s\n", argv[i], shader.warnings.empty() ? "" : "  avisos: ", shader.warnings.c_str());
        }
    }
    std::printf("%d de %d shaders compilan\n", total - failures, total);
    return failures != 0;
}
