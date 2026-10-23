#import <Metal/Metal.h>

#include "renderer.h"

#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "presenter.h"
#include "shader_translator.h"
#include "../memory.h"

namespace gpu
{
    namespace
    {
        // Registros de la GPU que usa el renderer (índices de 32 bits, según Xenia).
        constexpr uint32_t kRegSurfaceInfo = 0x2000;  // RB_SURFACE_INFO
        constexpr uint32_t kRegColorMask = 0x2104;    // RB_COLOR_MASK
        constexpr uint32_t kRegIndexOffset = 0x2102;  // VGT_INDX_OFFSET
        constexpr uint32_t kRegViewportXScale = 0x210F; // PA_CL_VPORT_XSCALE (y luego XOFFSET, YSCALE...)
        constexpr uint32_t kRegBlendControl0 = 0x2201; // RB_BLENDCONTROL0
        constexpr uint32_t kRegVteControl = 0x2206;   // PA_CL_VTE_CNTL
        constexpr uint32_t kRegWindowOffset = 0x2080; // PA_SC_WINDOW_OFFSET
        constexpr uint32_t kRegModeControl = 0x2205;  // PA_SU_SC_MODE_CNTL
        constexpr uint32_t kRegDepthInfo = 0x2002;    // RB_DEPTH_INFO (bit 16: profundidad en coma flotante)
        constexpr uint32_t kRegDepthControl = 0x2200; // RB_DEPTHCONTROL
        constexpr uint32_t kRegStencilBack = 0x210C;  // RB_STENCILREFMASK_BF
        constexpr uint32_t kRegStencilFront = 0x210D; // RB_STENCILREFMASK: ref 0-7, máscara 8-15, escritura 16-23
        constexpr uint32_t kRegDepthClear = 0x231D;   // RB_DEPTH_CLEAR: profundidad en 8-31, stencil en 0-7
        constexpr MTLPixelFormat kDepthFormat = MTLPixelFormatDepth32Float_Stencil8;

        // Desplazamiento de ventana (x, y con signo de 15 bits) si el modo lo aplica a los
        // vértices (bit 16). Con "predicated tiling" el juego dibuja la escena en franjas: cada
        // una se desplaza a la esquina de la EDRAM y un resolve la copia a su sitio.
        void WindowOffset(const uint32_t* registers, int32_t& x, int32_t& y)
        {
            x = y = 0;
            if (!(registers[kRegModeControl] & 0x10000))
                return;
            const uint32_t offset = registers[kRegWindowOffset];
            x = int32_t(offset << 17) >> 17;
            y = int32_t((offset >> 16) << 17) >> 17;
        }
        constexpr uint32_t kRegCopyControl = 0x2318;  // RB_COPY_CONTROL
        constexpr uint32_t kRegCopyDestBase = 0x2319;
        constexpr uint32_t kRegCopyDestPitch = 0x231A;
        constexpr uint32_t kRegCopyDestInfo = 0x231B;
        constexpr uint32_t kRegColorClear = 0x231E;   // RB_COLOR_CLEAR
        constexpr uint32_t kRegAluConstants = 0x4000; // 256 float4 de vértices + 256 de píxeles
        constexpr uint32_t kRegFetchConstants = 0x4800;
        constexpr uint32_t kRegBoolConstants = 0x4900;

        constexpr uint32_t kTargetWidth = 1280, kTargetHeight = 720;

        bool Trace() { return std::getenv("PROSTREET_TRACE") != nullptr; }

        float AsFloat(uint32_t bits)
        {
            float value;
            std::memcpy(&value, &bits, 4);
            return value;
        }

        struct Texture
        {
            id<MTLTexture> texture;
            uint64_t hash = 0; // huella de la memoria original (con mosaico, sin intercambiar)
        };

        // Imagen que ha escrito un resolve (con mosaico, 8_8_8_8): se guarda también una copia en
        // la GPU. Si un dibujo la usa luego como textura y la memoria no ha cambiado, se usa esa
        // copia en vez de deshacer el mosaico y volver a subirla. Con predicated tiling, cada
        // franja se escribe en su parte de la misma imagen.
        struct ResolvedSurface
        {
            uint32_t base = 0, pitch = 0, height = 0, endian = 0;
            id<MTLTexture> texture; // RGBA8 tal como está en el renderer
            id<MTLTexture> view;    // la misma, con R y B cambiados: lo que lee la textura del juego
            uint64_t hash = 0;      // de la memoria de toda la imagen tras el último resolve
        };

        // Bytes de la memoria original de una imagen con mosaico (las filas van de 32 en 32).
        size_t TiledSize(uint32_t pitchBlocks, uint32_t blocksY, uint32_t bytesPerBlock)
        {
            return size_t((pitchBlocks + 31) & ~31u) * ((blocksY + 31) & ~31u) * bytesPerBlock;
        }

        // Huella rápida de un bloque de memoria: palabras de 64 bits cada 32 bytes. Basta para
        // ver si una textura cambió (resolves, vídeos, texturas que se cargan) sin reconvertirla.
        uint64_t HashMemory(const uint8_t* data, size_t size)
        {
            uint64_t hash = 1469598103934665603ull ^ size;
            size_t i = 0;
            for (; i + 8 <= size; i += 32)
            {
                uint64_t word;
                std::memcpy(&word, data + i, 8);
                hash = (hash ^ word) * 1099511628211ull;
            }
            if (size >= 8)
            {
                uint64_t word;
                std::memcpy(&word, data + size - 8, 8);
                hash = (hash ^ word) * 1099511628211ull;
            }
            return hash;
        }

        struct Renderer
        {
            id<MTLDevice> device;
            id<MTLCommandQueue> queue;
            id<MTLBuffer> memory;      // memoria física del juego, sin copia
            id<MTLTexture> target;     // la "EDRAM": destino de dibujo
            id<MTLTexture> depth;      // y su profundidad / stencil
            bool depthClearPending = true;
            double clearDepth = 1.0;
            uint8_t clearStencil = 0;
            std::unordered_map<uint64_t, id<MTLDepthStencilState>> depthStates;
            id<MTLCommandBuffer> commands;
            id<MTLRenderCommandEncoder> encoder;
            bool clearPending = true;
            MTLClearColor clearColor = MTLClearColorMake(0, 0, 0, 1);

            std::unordered_map<uint64_t, id<MTLFunction>> functions;
            std::unordered_map<uint64_t, bool> failedShaders;
            std::unordered_map<uint64_t, std::vector<uint32_t>> textureSlots; // texturas de cada shader
            std::unordered_map<uint64_t, id<MTLRenderPipelineState>> pipelines;
            std::unordered_map<uint64_t, Texture> textures;
            std::vector<ResolvedSurface> surfaces;
            std::unordered_map<uint32_t, id<MTLSamplerState>> samplers;
            id<MTLTexture> missingTexture;

            uint64_t draws = 0, skipped = 0, resolves = 0;

            bool Initialize()
            {
                device = MTLCreateSystemDefaultDevice();
                if (!device)
                    return false;
                queue = [device newCommandQueue];
                memory = [device newBufferWithBytesNoCopy:memory::Translate(memory::kPhysicalBase) length:memory::kPhysicalSize
                    options:MTLResourceStorageModeShared deallocator:nil];

                MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                    width:kTargetWidth height:kTargetHeight mipmapped:NO];
                descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                descriptor.storageMode = MTLStorageModeShared;
                target = [device newTextureWithDescriptor:descriptor];
                MTLTextureDescriptor* depthDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kDepthFormat
                    width:kTargetWidth height:kTargetHeight mipmapped:NO];
                depthDescriptor.usage = MTLTextureUsageRenderTarget;
                depthDescriptor.storageMode = MTLStorageModePrivate;
                depth = [device newTextureWithDescriptor:depthDescriptor];

                // Textura de 1x1 magenta para formatos aún no soportados (se ven enseguida).
                MTLTextureDescriptor* small = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
                missingTexture = [device newTextureWithDescriptor:small];
                const uint32_t magenta = 0xFFFF00FF;
                [missingTexture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&magenta bytesPerRow:4];
                std::fprintf(stderr, "[render] Metal: %s\n", device.name.UTF8String);
                return memory != nil && target != nil;
            }

            id<MTLFunction> Function(const Shader& shader, bool pixel, std::vector<uint32_t>* slots)
            {
                auto it = functions.find(shader.hash);
                if (it != functions.end())
                {
                    if (slots)
                        *slots = textureSlots[shader.hash];
                    return it->second;
                }
                if (failedShaders.count(shader.hash))
                    return nil;

                TranslatedShader translated = TranslateShader(pixel, shader.code, shader.hash);
                NSError* error = nil;
                id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:translated.source.c_str()] options:nil error:&error];
                id<MTLFunction> function = [library newFunctionWithName:[NSString stringWithUTF8String:translated.entry.c_str()]];
                if (!function)
                {
                    std::fprintf(stderr, "[render] no compila %s: %s\n", translated.entry.c_str(), error ? error.localizedDescription.UTF8String : "?");
                    failedShaders[shader.hash] = true;
                    return nil;
                }
                if (!translated.warnings.empty())
                    std::fprintf(stderr, "[render] %s: %s\n", translated.entry.c_str(), translated.warnings.c_str());
                functions[shader.hash] = function;
                textureSlots[shader.hash] = translated.textures;
                if (slots)
                    *slots = translated.textures;
                return function;
            }

            static MTLBlendFactor BlendFactor(uint32_t value)
            {
                switch (value)
                {
                case 0: return MTLBlendFactorZero;
                case 1: return MTLBlendFactorOne;
                case 4: return MTLBlendFactorSourceColor;
                case 5: return MTLBlendFactorOneMinusSourceColor;
                case 6: return MTLBlendFactorSourceAlpha;
                case 7: return MTLBlendFactorOneMinusSourceAlpha;
                case 8: return MTLBlendFactorDestinationColor;
                case 9: return MTLBlendFactorOneMinusDestinationColor;
                case 10: return MTLBlendFactorDestinationAlpha;
                case 11: return MTLBlendFactorOneMinusDestinationAlpha;
                case 12: return MTLBlendFactorBlendColor;
                case 13: return MTLBlendFactorOneMinusBlendColor;
                case 14: return MTLBlendFactorBlendAlpha;
                case 15: return MTLBlendFactorOneMinusBlendAlpha;
                case 16: return MTLBlendFactorSourceAlphaSaturated;
                default: return MTLBlendFactorOne;
                }
            }

            static MTLBlendOperation BlendOperation(uint32_t value)
            {
                switch (value)
                {
                case 1: return MTLBlendOperationSubtract;
                case 2: return MTLBlendOperationMin;
                case 3: return MTLBlendOperationMax;
                case 4: return MTLBlendOperationReverseSubtract;
                default: return MTLBlendOperationAdd;
                }
            }

            id<MTLRenderPipelineState> Pipeline(const DrawCall& draw, std::vector<uint32_t>& slots)
            {
                const uint32_t blend = draw.registers[kRegBlendControl0];
                const uint32_t mask = draw.registers[kRegColorMask] & 0xF;
                uint64_t key = draw.vertexShader->hash * 31 + draw.pixelShader->hash;
                key = key * 1000003 + blend;
                key = key * 17 + mask;
                key = key * 3 + (draw.depthOnly ? 1 : 0);

                id<MTLFunction> vertex = Function(*draw.vertexShader, false, nullptr);
                id<MTLFunction> fragment = Function(*draw.pixelShader, true, &slots);
                if (!vertex || !fragment)
                    return nil;
                auto it = pipelines.find(key);
                if (it != pipelines.end())
                    return it->second;

                MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
                descriptor.vertexFunction = vertex;
                descriptor.fragmentFunction = fragment;
                MTLRenderPipelineColorAttachmentDescriptor* color = descriptor.colorAttachments[0];
                color.pixelFormat = target.pixelFormat;
                descriptor.depthAttachmentPixelFormat = kDepthFormat;
                descriptor.stencilAttachmentPixelFormat = kDepthFormat;
                color.writeMask = draw.depthOnly ? MTLColorWriteMaskNone : (mask & 1 ? MTLColorWriteMaskRed : 0) | (mask & 2 ? MTLColorWriteMaskGreen : 0) |
                                  (mask & 4 ? MTLColorWriteMaskBlue : 0) | (mask & 8 ? MTLColorWriteMaskAlpha : 0);
                // RB_BLENDCONTROL: srcblend 0-4, comb 5-7, destblend 8-12, y lo mismo para alfa desde el bit 16.
                const bool opaque = (blend & 0x1FFF) == 0x0001 && ((blend >> 16) & 0x1FFF) == 0x0001;
                color.blendingEnabled = !opaque;
                color.sourceRGBBlendFactor = BlendFactor(blend & 0x1F);
                color.rgbBlendOperation = BlendOperation((blend >> 5) & 7);
                color.destinationRGBBlendFactor = BlendFactor((blend >> 8) & 0x1F);
                color.sourceAlphaBlendFactor = BlendFactor((blend >> 16) & 0x1F);
                color.alphaBlendOperation = BlendOperation((blend >> 21) & 7);
                color.destinationAlphaBlendFactor = BlendFactor((blend >> 24) & 0x1F);

                NSError* error = nil;
                id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
                if (!pipeline)
                    std::fprintf(stderr, "[render] pipeline: %s\n", error.localizedDescription.UTF8String);
                pipelines[key] = pipeline;
                return pipeline;
            }

            // Prueba de profundidad y stencil (RB_DEPTHCONTROL). Las funciones de comparación y las
            // operaciones de stencil de Xenos van en el mismo orden que las de Metal.
            id<MTLDepthStencilState> DepthState(const uint32_t* registers)
            {
                const uint32_t control = registers[kRegDepthControl];
                const uint32_t front = registers[kRegStencilFront], back = registers[kRegStencilBack];
                const uint64_t key = (uint64_t(control) << 32) ^ (uint64_t(front & 0xFFFF00) << 8) ^ (back & 0xFFFF00);
                auto it = depthStates.find(key);
                if (it != depthStates.end())
                    return it->second;

                MTLDepthStencilDescriptor* descriptor = [[MTLDepthStencilDescriptor alloc] init];
                const bool zEnable = control & 2, zWrite = control & 4;
                descriptor.depthCompareFunction = zEnable ? MTLCompareFunction((control >> 4) & 7) : MTLCompareFunctionAlways;
                descriptor.depthWriteEnabled = zEnable && zWrite;
                if (control & 1)
                {
                    auto stencil = [](uint32_t bits, uint32_t refMask) {
                        MTLStencilDescriptor* side = [[MTLStencilDescriptor alloc] init];
                        side.stencilCompareFunction = MTLCompareFunction(bits & 7);
                        side.stencilFailureOperation = MTLStencilOperation((bits >> 3) & 7);
                        side.depthStencilPassOperation = MTLStencilOperation((bits >> 6) & 7);
                        side.depthFailureOperation = MTLStencilOperation((bits >> 9) & 7);
                        side.readMask = (refMask >> 8) & 0xFF;
                        side.writeMask = (refMask >> 16) & 0xFF;
                        return side;
                    };
                    descriptor.frontFaceStencil = stencil(control >> 8, front);
                    // Sin "backface_enable" (bit 7) la cara trasera usa lo mismo que la delantera.
                    descriptor.backFaceStencil = (control & 0x80) ? stencil(control >> 20, back) : stencil(control >> 8, front);
                }
                id<MTLDepthStencilState> state = [device newDepthStencilStateWithDescriptor:descriptor];
                depthStates[key] = state;
                return state;
            }

            // Textura a partir de una constante de fetch (6 palabras).
            id<MTLTexture> TextureFor(const uint32_t* fetch)
            {
                const bool tiled = fetch[0] >> 31;
                const uint32_t pitch = ((fetch[0] >> 22) & 0x1FF) * 32;
                const uint32_t format = fetch[1] & 0x3F, endian = (fetch[1] >> 6) & 3;
                const uint32_t base = fetch[1] & 0xFFFFF000;
                // Tamaño: 13 + 13 bits en 2D; en 3D, 11 + 11 (+ 10 de profundidad). De una textura 3D
                // (p. ej. una tabla de color) de momento se usa la primera capa como 2D.
                const uint32_t dimension = (fetch[5] >> 9) & 3;
                const uint32_t width = dimension == 2 ? (fetch[2] & 0x7FF) + 1 : (fetch[2] & 0x1FFF) + 1;
                const uint32_t height = dimension == 2 ? ((fetch[2] >> 11) & 0x7FF) + 1 : ((fetch[2] >> 13) & 0x1FFF) + 1;

                MTLPixelFormat pixelFormat;
                uint32_t blockSize = 1, bytesPerBlock;
                switch (format)
                {
                case 2: pixelFormat = MTLPixelFormatR8Unorm; bytesPerBlock = 1; break;
                case 10: pixelFormat = MTLPixelFormatRG8Unorm; bytesPerBlock = 2; break;
                case 6: pixelFormat = MTLPixelFormatRGBA8Unorm; bytesPerBlock = 4; break;
                case 18: pixelFormat = MTLPixelFormatBC1_RGBA; blockSize = 4; bytesPerBlock = 8; break;
                case 19: pixelFormat = MTLPixelFormatBC2_RGBA; blockSize = 4; bytesPerBlock = 16; break;
                case 20: pixelFormat = MTLPixelFormatBC3_RGBA; blockSize = 4; bytesPerBlock = 16; break;
                default:
                {
                    static std::unordered_map<uint32_t, int> reported;
                    if (!reported[format]++)
                        std::fprintf(stderr, "[render] formato de textura %u aún no soportado (se ve magenta)\n", format);
                    return missingTexture;
                }
                }
                if (base == 0 || width > 8192 || height > 8192)
                    return missingTexture;

                const uint32_t blocksX = (width + blockSize - 1) / blockSize, blocksY = (height + blockSize - 1) / blockSize;
                const uint32_t pitchBlocks = std::max(pitch / blockSize, blocksX);
                uint32_t log2Bytes = 0;
                while ((1u << log2Bytes) < bytesPerBlock)
                    log2Bytes++;

                // ¿Ha cambiado? Se mira la memoria original (con mosaico, las filas van de 32 en 32).
                const auto* source = static_cast<const uint8_t*>(memory::Translate(0xA0000000 | (base & 0x1FFFFFFF)));
                const size_t rawSize = std::min<size_t>(tiled ? TiledSize(pitchBlocks, blocksY, bytesPerBlock) :
                    size_t(pitchBlocks) * blocksY * bytesPerBlock, 0x20000000 - (base & 0x1FFFFFFF));
                const uint64_t hash = HashMemory(source, rawSize);
                if (format == 6 && tiled)
                    for (const ResolvedSurface& surface : surfaces)
                        if (surface.base == (base & 0x1FFFFFFF) && surface.pitch == pitchBlocks && surface.height == height &&
                            surface.endian == endian && width <= surface.pitch && surface.hash == hash)
                            return surface.view;
                const uint64_t key = (uint64_t(base) << 32) ^ (uint64_t(format) << 26) ^ (uint64_t(width) << 13) ^ height;
                Texture& cached = textures[key];
                if (cached.texture && cached.hash == hash)
                    return cached.texture;

                // Lectura (quitando el mosaico) e intercambio de bytes.
                std::vector<uint8_t> data(size_t(blocksX) * blocksY * bytesPerBlock);
                for (uint32_t by = 0; by < blocksY; by++)
                {
                    for (uint32_t bx = 0; bx < blocksX; bx++)
                    {
                        const uint32_t offset = tiled ? TiledOffset(bx, by, pitchBlocks, log2Bytes) : (by * pitchBlocks + bx) * bytesPerBlock;
                        std::memcpy(&data[(size_t(by) * blocksX + bx) * bytesPerBlock], source + offset, bytesPerBlock);
                    }
                }
                for (size_t i = 0; i + 3 < data.size(); i += 4)
                {
                    uint32_t word;
                    std::memcpy(&word, &data[i], 4);
                    switch (endian)
                    {
                    case 1: word = ((word & 0x00FF00FF) << 8) | ((word >> 8) & 0x00FF00FF); break;
                    case 2: word = __builtin_bswap32(word); break;
                    case 3: word = (word << 16) | (word >> 16); break;
                    default: break;
                    }
                    std::memcpy(&data[i], &word, 4);
                }

                // Textura nueva (no se reescribe la anterior: puede usarla un dibujo aún sin enviar).
                MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixelFormat
                    width:width height:height mipmapped:NO];
                cached.texture = [device newTextureWithDescriptor:descriptor];
                [cached.texture replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:data.data()
                    bytesPerRow:blocksX * bytesPerBlock];
                cached.hash = hash;
                if (Trace())
                    std::fprintf(stderr, "[render] textura 0x%08X %ux%u formato %u%s\n", base, width, height, format, tiled ? " (mosaico)" : "");
                return cached.texture;
            }

            id<MTLSamplerState> SamplerFor(const uint32_t* fetch)
            {
                // clamp_x/y: bits 10-12 y 13-15 de la palabra 0; filtros mag/min: bits 19-20 y 21-22 de la palabra 3.
                const uint32_t key = ((fetch[0] >> 10) & 0x3F) | (((fetch[3] >> 19) & 0xF) << 6);
                auto it = samplers.find(key);
                if (it != samplers.end())
                    return it->second;
                auto address = [](uint32_t clamp) {
                    switch (clamp)
                    {
                    case 0: return MTLSamplerAddressModeRepeat;
                    case 1: return MTLSamplerAddressModeMirrorRepeat;
                    default: return MTLSamplerAddressModeClampToEdge;
                    }
                };
                MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
                descriptor.sAddressMode = address((fetch[0] >> 10) & 7);
                descriptor.tAddressMode = address((fetch[0] >> 13) & 7);
                descriptor.magFilter = ((fetch[3] >> 19) & 3) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
                descriptor.minFilter = ((fetch[3] >> 21) & 3) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
                id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
                samplers[key] = sampler;
                return sampler;
            }

            void BeginPass()
            {
                if (encoder)
                    return;
                commands = [queue commandBuffer];
                MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = target;
                pass.colorAttachments[0].loadAction = clearPending ? MTLLoadActionClear : MTLLoadActionLoad;
                pass.colorAttachments[0].clearColor = clearColor;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.depthAttachment.texture = depth;
                pass.depthAttachment.loadAction = depthClearPending ? MTLLoadActionClear : MTLLoadActionLoad;
                pass.depthAttachment.clearDepth = clearDepth;
                pass.depthAttachment.storeAction = MTLStoreActionStore;
                pass.stencilAttachment.texture = depth;
                pass.stencilAttachment.loadAction = depthClearPending ? MTLLoadActionClear : MTLLoadActionLoad;
                pass.stencilAttachment.clearStencil = clearStencil;
                pass.stencilAttachment.storeAction = MTLStoreActionStore;
                clearPending = false;
                depthClearPending = false;
                encoder = [commands renderCommandEncoderWithDescriptor:pass];
            }

            void EndPass(bool wait)
            {
                if (encoder)
                {
                    [encoder endEncoding];
                    encoder = nil;
                }
                if (commands)
                {
                    [commands commit];
                    if (wait)
                        [commands waitUntilCompleted];
                    commands = nil;
                }
            }

            bool Draw(const DrawCall& draw)
            {
                // De momento: listas de triángulos, tiras, listas de rectángulos y de quads.
                const uint32_t primitive = draw.primitive;
                if ((primitive != 4 && primitive != 6 && primitive != 8 && primitive != 13) || draw.count == 0 ||
                    draw.vertexShader->code.empty() || draw.pixelShader->code.empty())
                {
                    skipped++;
                    return false;
                }
                std::vector<uint32_t> textureSlots;
                id<MTLRenderPipelineState> pipeline = Pipeline(draw, textureSlots);
                if (!pipeline)
                {
                    skipped++;
                    return false;
                }

                BeginPass();
                [encoder setRenderPipelineState:pipeline];
                [encoder setCullMode:MTLCullModeNone];
                [encoder setDepthStencilState:DepthState(draw.registers)];
                [encoder setStencilFrontReferenceValue:draw.registers[kRegStencilFront] & 0xFF
                    backReferenceValue:((draw.registers[kRegDepthControl] & 0x80) ? draw.registers[kRegStencilBack] : draw.registers[kRegStencilFront]) & 0xFF];
                // PA_SU_SC_MODE_CNTL bit 2: 0 = la cara delantera es la de sentido antihorario.
                [encoder setFrontFacingWinding:(draw.registers[kRegModeControl] & 4) ? MTLWindingClockwise : MTLWindingCounterClockwise];

                DrawState state{};
                state.primitive = primitive;
                state.indexSize = draw.indexSize;
                state.indexBase = draw.indexBase;
                state.indexEndian = draw.indexEndian;
                state.indexOffset = draw.registers[kRegIndexOffset];
                state.vte = draw.registers[kRegVteControl];
                for (int i = 0; i < 3; i++)
                {
                    state.viewportScale[i] = AsFloat(draw.registers[kRegViewportXScale + i * 2]);
                    state.viewportOffset[i] = AsFloat(draw.registers[kRegViewportXScale + i * 2 + 1]);
                }
                state.targetSize[0] = kTargetWidth;
                state.targetSize[1] = kTargetHeight;
                int32_t windowX, windowY;
                WindowOffset(draw.registers, windowX, windowY);
                state.windowOffset[0] = float(windowX);
                state.windowOffset[1] = float(windowY);
                std::memcpy(state.bools, &draw.registers[kRegBoolConstants], sizeof(state.bools));

                const uint32_t* vertexConstants = &draw.registers[kRegAluConstants];
                const uint32_t* pixelConstants = &draw.registers[kRegAluConstants + 256 * 4];
                const uint32_t* fetchConstants = &draw.registers[kRegFetchConstants];
                [encoder setVertexBuffer:memory offset:0 atIndex:0];
                [encoder setVertexBytes:vertexConstants length:256 * 16 atIndex:1];
                [encoder setVertexBytes:fetchConstants length:32 * 6 * 4 atIndex:2];
                [encoder setVertexBytes:&state length:sizeof(state) atIndex:3];
                [encoder setFragmentBuffer:memory offset:0 atIndex:0];
                [encoder setFragmentBytes:pixelConstants length:256 * 16 atIndex:1];
                [encoder setFragmentBytes:fetchConstants length:32 * 6 * 4 atIndex:2];
                [encoder setFragmentBytes:&state length:sizeof(state) atIndex:3];
                for (uint32_t slot : textureSlots)
                {
                    [encoder setFragmentTexture:TextureFor(&fetchConstants[slot * 6]) atIndex:slot];
                    [encoder setFragmentSamplerState:SamplerFor(&fetchConstants[slot * 6]) atIndex:slot];
                }

                const uint32_t vertices = primitive == 13 ? draw.count / 4 * 6 : primitive == 8 ? draw.count / 3 * 6 : draw.count;
                [encoder drawPrimitives:primitive == 6 ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle
                    vertexStart:0 vertexCount:vertices];
                draws++;
                return true;
            }

            // Copia en la GPU de lo que acaba de escribir un resolve (ver ResolvedSurface).
            void UpdateSurface(uint32_t base, uint32_t pitch, uint32_t height, uint32_t endian,
                uint32_t left, uint32_t top, uint32_t width, uint32_t rows)
            {
                // ¿Es una franja de una imagen ya conocida? Las filas de 32 en 32 son contiguas.
                ResolvedSurface* surface = nullptr;
                uint32_t offsetY = 0;
                const uint32_t macroRow = pitch * 4 * 32;
                for (ResolvedSurface& candidate : surfaces)
                {
                    const uint32_t size = candidate.pitch * 4 * candidate.height;
                    if (candidate.pitch == pitch && base >= candidate.base && base < candidate.base + size &&
                        (base - candidate.base) % macroRow == 0)
                    {
                        surface = &candidate;
                        offsetY = (base - candidate.base) / (pitch * 4);
                        break;
                    }
                }
                if (!surface || offsetY + top + rows > surface->height)
                {
                    if (surface)
                        surfaces.erase(surfaces.begin() + (surface - surfaces.data()));
                    if (surfaces.size() >= 32)
                        surfaces.erase(surfaces.begin());
                    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                        width:pitch height:height mipmapped:NO];
                    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
                    descriptor.storageMode = MTLStorageModePrivate;
                    ResolvedSurface created;
                    created.base = base;
                    created.pitch = pitch;
                    created.height = height;
                    created.texture = [device newTextureWithDescriptor:descriptor];
                    created.view = [created.texture newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm textureType:MTLTextureType2D
                        levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)
                        swizzle:MTLTextureSwizzleChannelsMake(MTLTextureSwizzleBlue, MTLTextureSwizzleGreen,
                            MTLTextureSwizzleRed, MTLTextureSwizzleAlpha)];
                    surfaces.push_back(created);
                    surface = &surfaces.back();
                    offsetY = 0;
                }
                surface->endian = endian;

                id<MTLCommandBuffer> copy = [queue commandBuffer];
                id<MTLBlitCommandEncoder> blit = [copy blitCommandEncoder];
                [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(left, top, 0)
                    sourceSize:MTLSizeMake(width, rows, 1) toTexture:surface->texture destinationSlice:0 destinationLevel:0
                    destinationOrigin:MTLOriginMake(left, offsetY + top, 0)];
                [blit endEncoding];
                [copy commit];

                const auto* memoryStart = static_cast<const uint8_t*>(memory::Translate(0xA0000000 | surface->base));
                surface->hash = HashMemory(memoryStart, std::min<size_t>(TiledSize(pitch, surface->height, 4),
                    0x20000000 - surface->base));
            }

            // Rectángulo del resolve: 3 vértices (x, y en float) de la constante de fetch 0, más el
            // desplazamiento de ventana. Si no parece válido se deja el que viene (todo el destino).
            void ResolveRect(const uint32_t* registers, uint32_t& left, uint32_t& top, uint32_t& right, uint32_t& bottom)
            {
                const uint32_t vertices = 0xA0000000 | (registers[kRegFetchConstants] & 0x1FFFFFFC);
                float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
                for (int i = 0; i < 3; i++)
                {
                    const float x = AsFloat(__builtin_bswap32(*static_cast<const uint32_t*>(memory::Translate(vertices + i * 8))));
                    const float y = AsFloat(__builtin_bswap32(*static_cast<const uint32_t*>(memory::Translate(vertices + i * 8 + 4))));
                    minX = std::min(minX, x); maxX = std::max(maxX, x);
                    minY = std::min(minY, y); maxY = std::max(maxY, y);
                }
                int32_t windowX, windowY;
                WindowOffset(registers, windowX, windowY);
                minX += windowX; maxX += windowX; minY += windowY; maxY += windowY;
                if (!(minX >= 0 && minY >= 0 && maxX > minX && maxY > minY && maxX <= 8192 && maxY <= 8192))
                    return;
                left = uint32_t(minX); top = uint32_t(minY);
                right = uint32_t(maxX); bottom = uint32_t(maxY);
            }

            void Resolve(const uint32_t* registers)
            {
                resolves++;
                const uint32_t control = registers[kRegCopyControl];
                const uint32_t destBase = registers[kRegCopyDestBase];
                const uint32_t destPitch = registers[kRegCopyDestPitch] & 0x3FFF;
                const uint32_t destHeight = (registers[kRegCopyDestPitch] >> 16) & 0x3FFF;
                const uint32_t destInfo = registers[kRegCopyDestInfo];
                const uint32_t destEndian = destInfo & 7, destFormat = (destInfo >> 7) & 0x3F;
                const uint32_t source = control & 7;

                static const bool trace = std::getenv("PROSTREET_GPU_RESOLVES") != nullptr;
                if (trace)
                {
                    // El rectángulo del resolve son 3 vértices (x, y en float) de la constante de fetch 0.
                    const uint32_t vertices = 0xA0000000 | (registers[kRegFetchConstants] & 0x1FFFFFFC);
                    float rect[6];
                    for (int i = 0; i < 6; i++)
                        rect[i] = AsFloat(__builtin_bswap32(*static_cast<const uint32_t*>(memory::Translate(vertices + i * 4))));
                    std::fprintf(stderr, "[resolve] control 0x%X destino 0x%08X pitch %u alto %u info 0x%08X | ventana 0x%08X "
                        "recorte 0x%08X-0x%08X superficie 0x%08X color 0x%08X modo 0x%08X | rect (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f)\n",
                        control, destBase, destPitch, destHeight, destInfo, registers[0x2080], registers[0x2081],
                        registers[0x2082], registers[kRegSurfaceInfo], registers[0x2001], registers[0x2205],
                        rect[0], rect[1], rect[2], rect[3], rect[4], rect[5]);
                }

                // Copiar la EDRAM a memoria (solo color, formato 8_8_8_8 por ahora). El rectángulo
                // está en coordenadas de pantalla: con el desplazamiento de ventana pasa a las de
                // la EDRAM, que son también las del destino (cuya base ya apunta a la franja).
                uint32_t left = 0, top = 0, right = destPitch, bottom = destHeight;
                ResolveRect(registers, left, top, right, bottom);
                right = std::min({right, destPitch, kTargetWidth});
                bottom = std::min({bottom, destHeight, kTargetHeight});
                if (source < 4 && destFormat == 6 && destBase && right > left && bottom > top)
                {
                    EndPass(true);
                    const uint32_t width = right - left, height = bottom - top;
                    std::vector<uint32_t> pixels(size_t(width) * height);
                    [target getBytes:pixels.data() bytesPerRow:width * 4 fromRegion:MTLRegionMake2D(left, top, width, height) mipmapLevel:0];
                    auto* dest = static_cast<uint8_t*>(memory::Translate(0xA0000000 | (destBase & 0x1FFFFFFF)));
                    for (uint32_t y = 0; y < height; y++)
                    {
                        for (uint32_t x = 0; x < width; x++)
                        {
                            // RGBA8 de Metal (R en el byte bajo) -> palabra 0xAARRGGBB en memoria.
                            const uint32_t rgba = pixels[size_t(y) * width + x];
                            uint32_t value = (rgba & 0xFF00FF00) | ((rgba & 0xFF) << 16) | ((rgba >> 16) & 0xFF);
                            switch (destEndian)
                            {
                            case 1: value = ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF); break;
                            case 2: value = __builtin_bswap32(value); break;
                            case 3: value = (value << 16) | (value >> 16); break;
                            default: break;
                            }
                            std::memcpy(dest + TiledOffset(left + x, top + y, destPitch, 2), &value, 4);
                        }
                    }
                    UpdateSurface(destBase & 0x1FFFFFFF, destPitch, destHeight, destEndian, left, top, width, height);
                }

                // Borrado tras la copia (RB_COPY_CONTROL bit 8): en la siguiente pasada.
                if (control & 0x100)
                {
                    EndPass(false);
                    const uint32_t clear = registers[kRegColorClear];
                    clearColor = MTLClearColorMake(((clear >> 16) & 0xFF) / 255.0, ((clear >> 8) & 0xFF) / 255.0,
                        (clear & 0xFF) / 255.0, ((clear >> 24) & 0xFF) / 255.0);
                    clearPending = true;
                }
                // Borrado de profundidad / stencil (bit 9). D24S8 o D24FS8 (20e4) según RB_DEPTH_INFO.
                if (control & 0x200)
                {
                    EndPass(false);
                    const uint32_t clear = registers[kRegDepthClear], value = clear >> 8;
                    if (registers[kRegDepthInfo] & 0x10000)
                    {
                        const uint32_t exponent = value >> 20, mantissa = value & 0xFFFFF;
                        clearDepth = exponent ? std::ldexp(1.0 + mantissa / 1048576.0, int(exponent) - 15) : std::ldexp(mantissa / 1048576.0, -14);
                        clearDepth = std::min(clearDepth, 1.0);
                    }
                    else
                    {
                        clearDepth = value / 16777215.0;
                    }
                    clearStencil = uint8_t(clear & 0xFF);
                    depthClearPending = true;
                }
            }
        };

        Renderer* gRenderer = nullptr;
        bool gUnavailable = false;

        Renderer* Get()
        {
            if (!gRenderer && !gUnavailable)
            {
                gRenderer = new Renderer();
                if (!gRenderer->Initialize())
                {
                    std::fprintf(stderr, "[render] Metal no está disponible: no se dibujará nada\n");
                    delete gRenderer;
                    gRenderer = nullptr;
                    gUnavailable = true;
                }
            }
            return gRenderer;
        }
    }

    bool RendererDraw(const DrawCall& draw)
    {
        @autoreleasepool
        {
            Renderer* renderer = Get();
            return renderer && renderer->Draw(draw);
        }
    }

    void RendererResolve(const uint32_t* registers)
    {
        @autoreleasepool
        {
            if (Renderer* renderer = Get())
                renderer->Resolve(registers);
        }
    }
}
