#import <Metal/Metal.h>

#include "renderer.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
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
        constexpr uint32_t kRegColorControl = 0x2202;  // RB_COLORCONTROL (prueba alfa)
        constexpr uint32_t kRegAlphaRef = 0x210E;      // RB_ALPHA_REF
        constexpr uint32_t kRegVteControl = 0x2206;   // PA_CL_VTE_CNTL
        constexpr uint32_t kRegPointSize = 0x2280;    // PA_SU_POINT_SIZE: alto 0-15, ancho 16-31
        constexpr uint32_t kRegPointMinMax = 0x2281;  // PA_SU_POINT_MINMAX: mínimo 0-15, máximo 16-31
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

        // Como la EDRAM, cabe cualquier superficie del juego: la pantalla (1280x720) y el mapa de
        // sombras (1600x1600, solo profundidad).
        constexpr uint32_t kTargetWidth = 2048, kTargetHeight = 2048;
        // Destino con MSAA: la escena del juego (1280x720, que con 4 muestras va por franjas).
        constexpr uint32_t kMsaaWidth = 1280, kMsaaHeight = 720;
        // Las ranuras de textura de un shader llevan este bit si las lee como mapa cúbico.
        constexpr uint32_t kCubeSlot = 0x100;

        bool Trace() { return std::getenv("PROSTREET_TRACE") != nullptr; }

        // Escala de resolución pedida (ver SetResolutionScale): el renderer la aplica al presentar.
        std::atomic<float> gRequestedScale{ std::getenv("PROSTREET_SCALE") ? std::clamp(float(std::atof(std::getenv("PROSTREET_SCALE"))), 1.0f, 2.0f) : 1.0f };

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
            uint64_t epoch = 0; // época en la que se comprobó por última vez (ver Renderer::epoch)
        };

        // Imagen que ha escrito un resolve (con mosaico, 8_8_8_8): se guarda también una copia en
        // la GPU (a la escala de resolución; base, pitch y height son los del juego). Si un dibujo la usa luego como textura y la memoria no ha cambiado, se usa esa
        // copia en vez de deshacer el mosaico y volver a subirla. Con predicated tiling, cada
        // franja se escribe en su parte de la misma imagen.
        struct ResolvedSurface
        {
            uint32_t base = 0, pitch = 0, height = 0, endian = 0;
            id<MTLTexture> texture; // RGBA8 tal como está en el renderer
            id<MTLTexture> view;    // la misma, con R y B cambiados: lo que lee la textura del juego
            uint64_t hash = 0;      // de la memoria de toda la imagen tras el último resolve
            // Escritura diferida: la memoria del juego aún no tiene este rectángulo (en filas de la imagen).
            bool pending = false;
            uint64_t validEpoch = 0; // época en la que su memoria se comprobó (hash) por última vez
            uint32_t pendingLeft = 0, pendingTop = 0, pendingRight = 0, pendingBottom = 0;
            uint64_t version = 0;    // cambia con cada resolve que la escribe
        };

        // Mapa cúbico montado en la GPU con las 6 imágenes que han dejado los resolves (p. ej. el de
        // los reflejos, que se redibuja en cada fotograma): así no hay que leerlas de la GPU.
        struct SurfaceCube
        {
            id<MTLTexture> texture, view; // view: con R y B cambiados, como ResolvedSurface::view
            uint64_t versions[6] = {};
        };

        // Profundidad que ha copiado un resolve (fuente 4). Solo está en la GPU (R32Float): el juego
        // la lee como textura 24_8 en la misma dirección. Con predicated tiling, cada franja
        // llega con su propia dirección dentro de la imagen.
        struct DepthSurface
        {
            uint32_t base = 0, pitch = 0, height = 0;
            id<MTLTexture> texture;
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
            // La "EDRAM": destinos de dibujo. El 0 es sin MSAA y cabe cualquier superficie; el 1 es para
            // las superficies con MSAA (RB_SURFACE_INFO bits 16-17, p. ej. la escena a 1280 de ancho):
            // se dibuja con 4 muestras y al cerrar cada pasada se resuelve en color/depth, que es lo que
            // leen los resolves. PROSTREET_NO_MSAA=1: todo al destino 0.
            // Las texturas van a la escala de resolución; width y height son los del juego (los
            // dibujos y resolves usan esas coordenadas y se pasan a las de las texturas con Scaled).
            struct RenderSet
            {
                id<MTLTexture> color, depth;     // 1 muestra
                id<MTLTexture> colorMS, depthMS; // con MSAA: donde se dibuja
                uint32_t width = 0, height = 0, samples = 1;
                bool clearPending = true, depthClearPending = true;
                MTLClearColor clearColor = MTLClearColorMake(0, 0, 0, 1);
                double clearDepth = 1.0;
                uint8_t clearStencil = 0;
            };
            RenderSet sets[2];
            uint32_t passSet = 0; // el de la pasada abierta
            const bool msaaEnabled = std::getenv("PROSTREET_NO_MSAA") == nullptr;
            float scale = 1.0f; // escala de resolución en uso

            // Coordenada del juego (píxeles de la EDRAM o de una imagen de resolve) en las texturas.
            uint32_t Scaled(uint32_t value) const { return uint32_t(value * scale + 0.5f); }

            uint32_t SetFor(const uint32_t* registers) const
            {
                const uint32_t info = registers[kRegSurfaceInfo];
                return msaaEnabled && sets[1].color && ((info >> 16) & 3) && (info & 0x3FFF) <= sets[1].width ? 1 : 0;
            }
            std::unordered_map<uint64_t, id<MTLDepthStencilState>> depthStates;
            id<MTLCommandBuffer> commands;
            id<MTLRenderCommandEncoder> encoder;

            std::unordered_map<uint64_t, id<MTLFunction>> functions;
            std::unordered_map<uint64_t, bool> failedShaders;
            std::unordered_map<uint64_t, std::vector<uint32_t>> textureSlots; // texturas de cada shader
            std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> constantRanges; // de cada shader
            std::unordered_map<uint64_t, id<MTLRenderPipelineState>> pipelines;
            std::unordered_map<uint64_t, Texture> textures;
            std::vector<ResolvedSurface> surfaces;
            std::unordered_map<uint32_t, SurfaceCube> surfaceCubes;
            uint64_t surfaceVersions = 0;
            std::vector<DepthSurface> depthSurfaces;
            id<MTLBuffer> depthStaging; // profundidad de camino a una DepthSurface

            // Por ranura de textura: la última constante de fetch vista, su textura, su sampler y en
            // qué época. Muchos dibujos seguidos usan las mismas texturas.
            struct SlotMemo
            {
                uint32_t fetch[6] = {};
                uint64_t epoch = 0;
                bool cube = false;
                id<MTLTexture> texture;
                id<MTLSamplerState> sampler;
            };
            SlotMemo slotMemos[32];
            uint64_t lastPipelineKey = 0;
            id<MTLRenderPipelineState> lastPipeline;
            std::vector<uint32_t> lastPipelineSlots;
            id<MTLCommandBuffer> lastCommit;   // el último trabajo enviado a la GPU, para esperarlo
            // Época: cambia con cada resolve y cada fotograma presentado. Una textura ya comprobada
            // en la época actual no se vuelve a comprobar (su memoria solo cambia entre medias).
            uint64_t epoch = 1;
            // PROSTREET_RESOLVE_WRITEBACK=1: escribir cada resolve en la memoria del juego en el acto.
            const bool immediateWriteback = std::getenv("PROSTREET_RESOLVE_WRITEBACK") != nullptr;
            std::unordered_map<uint32_t, id<MTLSamplerState>> samplers;
            id<MTLTexture> missingTexture;
            id<MTLTexture> missingCube;    // lo mismo, para un shader que espera un mapa cúbico
            id<MTLTexture> blackTexture;   // (0, 0, 0, 0): constante de fetch sin textura (formato 0)
            id<MTLTexture> farDepthTexture; // profundidad 1 (lejos): 24_8 sin resolve que la haya escrito

            uint64_t draws = 0, skipped = 0, resolves = 0;

            // Constantes de los shaders (ALU de vértices y de píxeles, y de fetch) en un búfer
            // circular: se copian solo cuando cambian, no en cada dibujo.
            static constexpr size_t kRingSize = 64u << 20;
            id<MTLBuffer> constantRing;
            size_t ringOffset = 0;
            id<MTLCommandBuffer> halfCommit[2]; // último trabajo que usó cada mitad del búfer
            uint64_t uploadedVersion = 0;
            uint64_t uploadedVertexShader = 0, uploadedPixelShader = 0;

            struct ConstantOffsets
            {
                size_t vertex = 0, pixel = 0, fetch = 0; // desplazamientos a enlazar (buffer 1 y 2)
            };
            ConstantOffsets constantOffsets;

            // Reserva espacio para los registros [first, last] de un banco de constantes de 16 bytes y
            // devuelve el desplazamiento a enlazar (alineado a 256) con el que c[first] cae en lo reservado.
            size_t Allocate(uint32_t first, uint32_t last, uint32_t stride)
            {
                const size_t low = size_t(first) * stride, high = size_t(last + 1) * stride;
                const size_t oldHalf = std::min<size_t>(ringOffset * 2 / kRingSize, 1);
                size_t bind = ringOffset > low ? (ringOffset - low + 255) & ~size_t(255) : 0;
                if (bind + high > kRingSize) // vuelta al principio
                    bind = 0;
                // Al entrar en la otra mitad del búfer se espera al trabajo que la usó la vez anterior
                // (normalmente ya terminado); el actual se envía para poder esperarlo después.
                const size_t newHalf = (bind + high - 1) * 2 / kRingSize;
                if (newHalf != oldHalf)
                {
                    EndPass(false);
                    halfCommit[oldHalf] = lastCommit;
                    if (halfCommit[newHalf] && halfCommit[newHalf] != lastCommit)
                        [halfCommit[newHalf] waitUntilCompleted];
                    halfCommit[newHalf] = nil;
                }
                ringOffset = bind + high;
                return bind;
            }

            // Solo se copian los rangos de constantes que usan los shaders del dibujo.
            const ConstantOffsets& UploadConstants(const DrawCall& draw)
            {
                const uint64_t vertexShader = draw.vertexShader->hash, pixelShader = draw.pixelShader->hash;
                if (draw.constantsVersion == uploadedVersion && vertexShader == uploadedVertexShader && pixelShader == uploadedPixelShader)
                    return constantOffsets;
                auto* ring = static_cast<uint8_t*>(constantRing.contents);
                auto upload = [&](const std::vector<std::pair<uint32_t, uint32_t>>& ranges, uint32_t bank) -> size_t {
                    if (ranges.empty())
                        return 0;
                    const size_t bind = Allocate(ranges.front().first, ranges.back().first + ranges.back().second - 1, 16);
                    for (const auto& [first, count] : ranges)
                        std::memcpy(ring + bind + first * 16, &draw.registers[kRegAluConstants + bank * 4 + first * 4], count * 16);
                    return bind;
                };
                constantOffsets.vertex = upload(constantRanges[vertexShader], 0);
                constantOffsets.pixel = upload(constantRanges[pixelShader], 256);
                constantOffsets.fetch = Allocate(0, 0, 32 * 6 * 4);
                std::memcpy(ring + constantOffsets.fetch, &draw.registers[kRegFetchConstants], 32 * 6 * 4);
                uploadedVertexShader = vertexShader;
                uploadedPixelShader = pixelShader;
                uploadedVersion = draw.constantsVersion;
                return constantOffsets;
            }

            // Los destinos de dibujo (y el búfer de paso de la profundidad), a la escala actual.
            void CreateTargets()
            {
                auto createSet = [&](RenderSet& set, uint32_t width, uint32_t height, uint32_t samples) {
                    set.width = width;
                    set.height = height;
                    set.samples = samples;
                    set.clearPending = set.depthClearPending = true;
                    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                        width:Scaled(width) height:Scaled(height) mipmapped:NO];
                    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                    descriptor.storageMode = MTLStorageModeShared; // los resolves inmediatos la leen
                    set.color = [device newTextureWithDescriptor:descriptor];
                    MTLTextureDescriptor* depthDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kDepthFormat
                        width:Scaled(width) height:Scaled(height) mipmapped:NO];
                    depthDescriptor.usage = MTLTextureUsageRenderTarget;
                    depthDescriptor.storageMode = MTLStorageModePrivate;
                    set.depth = [device newTextureWithDescriptor:depthDescriptor];
                    if (samples > 1)
                    {
                        descriptor.textureType = MTLTextureType2DMultisample;
                        descriptor.sampleCount = samples;
                        descriptor.usage = MTLTextureUsageRenderTarget;
                        descriptor.storageMode = MTLStorageModePrivate;
                        set.colorMS = [device newTextureWithDescriptor:descriptor];
                        depthDescriptor.textureType = MTLTextureType2DMultisample;
                        depthDescriptor.sampleCount = samples;
                        set.depthMS = [device newTextureWithDescriptor:depthDescriptor];
                    }
                };
                createSet(sets[0], kTargetWidth, kTargetHeight, 1);
                if (msaaEnabled && [device supportsTextureSampleCount:4])
                    createSet(sets[1], kMsaaWidth, kMsaaHeight, 4);

                depthStaging = [device newBufferWithLength:size_t(Scaled(kTargetWidth)) * Scaled(kTargetHeight) * 4 options:MTLResourceStorageModePrivate];
            }

            // Cambio de escala (al presentar: la EDRAM ya no hace falta). Lo que solo está en la GPU
            // se escribe antes en la memoria del juego; las copias en la GPU se descartan.
            void ApplyScale()
            {
                const float requested = gRequestedScale.load();
                if (requested == scale)
                    return;
                EndPass(true);
                for (ResolvedSurface& surface : surfaces)
                    WritebackSurface(surface);
                surfaces.clear();
                surfaceCubes.clear();
                depthSurfaces.clear();
                scale = requested;
                CreateTargets();
                epoch++;
                std::fprintf(stderr, "[render] escala de resolución %.2gx (%ux%u)\n", scale, Scaled(kMsaaWidth), Scaled(kMsaaHeight));
            }

            bool Initialize()
            {
                device = MTLCreateSystemDefaultDevice();
                if (!device)
                    return false;
                queue = [device newCommandQueue];
                memory = [device newBufferWithBytesNoCopy:memory::Translate(memory::kPhysicalBase) length:memory::kPhysicalSize
                    options:MTLResourceStorageModeShared deallocator:nil];

                scale = gRequestedScale.load();
                CreateTargets();

                // Textura de 1x1 magenta para formatos aún no soportados (se ven enseguida).
                MTLTextureDescriptor* small = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
                missingTexture = [device newTextureWithDescriptor:small];
                const uint32_t magenta = 0xFFFF00FF;
                [missingTexture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&magenta bytesPerRow:4];
                missingCube = [device newTextureWithDescriptor:[MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                    size:1 mipmapped:NO]];
                for (NSUInteger face = 0; face < 6; face++)
                    [missingCube replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:face withBytes:&magenta bytesPerRow:4 bytesPerImage:4];
                blackTexture = [device newTextureWithDescriptor:small];
                const uint32_t black = 0;
                [blackTexture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&black bytesPerRow:4];
                farDepthTexture = [device newTextureWithDescriptor:[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                    width:1 height:1 mipmapped:NO]];
                const float far = 1.0f;
                [farDepthTexture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&far bytesPerRow:4];
                constantRing = [device newBufferWithLength:kRingSize options:MTLResourceStorageModeShared];
                std::fprintf(stderr, "[render] Metal: %s\n", device.name.UTF8String);
                return memory != nil && sets[0].color != nil;
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
                std::vector<uint32_t> shaderSlots = translated.textures;
                for (uint32_t& slot : shaderSlots)
                    if (std::find(translated.cubeTextures.begin(), translated.cubeTextures.end(), slot) != translated.cubeTextures.end())
                        slot |= kCubeSlot;
                functions[shader.hash] = function;
                textureSlots[shader.hash] = shaderSlots;
                constantRanges[shader.hash] = translated.constantRanges;
                if (slots)
                    *slots = shaderSlots;
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
                // Con MSAA, alpha-to-mask (RB_COLORCONTROL bit 4) es la cobertura por alfa de Metal.
                const RenderSet& set = sets[SetFor(draw.registers)];
                const bool alphaToCoverage = set.samples > 1 && (draw.registers[kRegColorControl] & 0x10);
                key = key * 7 + set.samples * 2 + (alphaToCoverage ? 1 : 0);
                if (key == lastPipelineKey && lastPipeline) // lo más habitual: el mismo que el dibujo anterior
                {
                    slots = lastPipelineSlots;
                    return lastPipeline;
                }

                id<MTLFunction> vertex = Function(*draw.vertexShader, false, nullptr);
                id<MTLFunction> fragment = Function(*draw.pixelShader, true, &slots);
                if (!vertex || !fragment)
                    return nil;
                auto it = pipelines.find(key);
                if (it != pipelines.end())
                {
                    lastPipelineKey = key;
                    lastPipeline = it->second;
                    lastPipelineSlots = slots;
                    return it->second;
                }

                MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
                descriptor.vertexFunction = vertex;
                descriptor.fragmentFunction = fragment;
                MTLRenderPipelineColorAttachmentDescriptor* color = descriptor.colorAttachments[0];
                color.pixelFormat = set.color.pixelFormat;
                descriptor.rasterSampleCount = set.samples;
                descriptor.alphaToCoverageEnabled = alphaToCoverage;
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

            // Cubo con las caras en [base + cara * faceStride] si las 6 son imágenes de resolves vigentes
            // (su memoria no ha cambiado desde entonces). Si alguna ha cambiado, se monta un cubo nuevo (el
            // anterior puede usarlo un dibujo aún sin enviar); la copia va antes que el trabajo pendiente.
            id<MTLTexture> CubeFromSurfaces(uint32_t base, size_t faceStride, uint32_t pitch, uint32_t size, uint32_t endian)
            {
                ResolvedSurface* faces[6];
                for (uint32_t face = 0; face < 6; face++)
                {
                    faces[face] = nullptr;
                    for (ResolvedSurface& surface : surfaces)
                        if (surface.base == base + face * faceStride && surface.pitch == pitch && surface.height == size &&
                            surface.endian == endian && size <= surface.pitch)
                        {
                            faces[face] = &surface;
                            break;
                        }
                    ResolvedSurface* surface = faces[face];
                    if (!surface)
                        return nil;
                    if (surface->validEpoch != epoch)
                    {
                        const auto* memoryStart = static_cast<const uint8_t*>(memory::Translate(0xA0000000 | surface->base));
                        if (surface->hash != HashMemory(memoryStart, std::min<size_t>(TiledSize(surface->pitch, surface->height, 4),
                            0x20000000 - surface->base)))
                            return nil;
                        surface->validEpoch = epoch;
                    }
                }
                SurfaceCube& cube = surfaceCubes[base];
                const uint32_t scaledSize = Scaled(size);
                bool changed = !cube.texture || cube.texture.width != scaledSize;
                for (uint32_t face = 0; face < 6; face++)
                    changed |= cube.versions[face] != faces[face]->version;
                if (!changed)
                    return cube.view;
                MTLTextureDescriptor* descriptor = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                    size:scaledSize mipmapped:NO];
                descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
                descriptor.storageMode = MTLStorageModePrivate;
                cube.texture = [device newTextureWithDescriptor:descriptor];
                cube.view = [cube.texture newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm textureType:MTLTextureTypeCube
                    levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 6)
                    swizzle:MTLTextureSwizzleChannelsMake(MTLTextureSwizzleBlue, MTLTextureSwizzleGreen,
                        MTLTextureSwizzleRed, MTLTextureSwizzleAlpha)];
                id<MTLCommandBuffer> copy = [queue commandBuffer];
                id<MTLBlitCommandEncoder> blit = [copy blitCommandEncoder];
                for (uint32_t face = 0; face < 6; face++)
                {
                    [blit copyFromTexture:faces[face]->texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake(scaledSize, scaledSize, 1) toTexture:cube.texture destinationSlice:face destinationLevel:0
                        destinationOrigin:MTLOriginMake(0, 0, 0)];
                    cube.versions[face] = faces[face]->version;
                }
                [blit endEncoding];
                [copy commit];
                return cube.view;
            }

            // Textura a partir de una constante de fetch (6 palabras). cube: el shader la lee como mapa
            // cúbico (entonces hay que darle uno, aunque sea el de relleno).
            id<MTLTexture> TextureFor(const uint32_t* fetch, bool cube)
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
                if (cube && dimension != 3)
                {
                    static bool reported = false;
                    if (!reported)
                        std::fprintf(stderr, "[render] un shader lee como cubo una textura de dimensión %u (se ve magenta)\n", dimension);
                    reported = true;
                    return missingCube;
                }
                // Un mapa cúbico tiene sus 6 caras seguidas; leído como 2D se usa la primera.
                const uint32_t faces = cube ? 6 : 1;

                if (format == 22 || format == 23) // 24_8 y 24_8_FLOAT: profundidad
                {
                    if (cube)
                        return missingCube;
                    for (const DepthSurface& surface : depthSurfaces)
                        if (surface.base == (base & 0x1FFFFFFF))
                            return surface.texture;
                    return farDepthTexture;
                }
                if (format == 0) // 1_REVERSE: en la práctica, una constante sin textura
                {
                    static bool reported = false;
                    if (!reported)
                        std::fprintf(stderr, "[render] textura de formato 0 (%08X %08X %08X %08X %08X %08X): se lee como (0, 0, 0, 0)\n",
                            fetch[0], fetch[1], fetch[2], fetch[3], fetch[4], fetch[5]);
                    reported = true;
                    return cube ? missingCube : blackTexture;
                }

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
                case 49: pixelFormat = MTLPixelFormatBC5_RGUnorm; blockSize = 4; bytesPerBlock = 16; break; // DXN
                default:
                {
                    static std::unordered_map<uint32_t, int> reported;
                    if (!reported[format]++)
                        std::fprintf(stderr, "[render] formato de textura %u aún no soportado (se ve magenta)\n", format);
                    return cube ? missingCube : missingTexture;
                }
                }
                if (base == 0 || width > 8192 || height > 8192 || (cube && width != height))
                    return cube ? missingCube : missingTexture;

                const uint32_t blocksX = (width + blockSize - 1) / blockSize, blocksY = (height + blockSize - 1) / blockSize;
                const uint32_t pitchBlocks = std::max(pitch / blockSize, blocksX);
                uint32_t log2Bytes = 0;
                while ((1u << log2Bytes) < bytesPerBlock)
                    log2Bytes++;

                // Niveles de mipmap (como Xenia): el 0 está en la base; del 1 en adelante, seguidos desde
                // mip_address (palabra 5), cada uno con el tamaño de la potencia de dos siguiente a la base
                // dividida entre 2^nivel y alineado a bloques de 32x32 (o filas de 256 bytes sin mosaico).
                // Con packed_mips, los niveles cuyo lado menor es de 16 texels o menos comparten un mismo
                // bloque de 32x32 (la "cola"), cada uno en su desplazamiento.
                const uint32_t mipAddress = fetch[5] & 0xFFFFF000;
                const bool packedMips = (fetch[5] >> 11) & 1;
                auto log2Ceil = [](uint32_t value) { uint32_t n = 0; while ((1u << n) < value) n++; return n; };
                const uint32_t fullChain = log2Ceil(std::max(width, height)) + 1;
                const uint32_t maxLevel = (fetch[4] >> 6) & 0xF;
                const uint32_t levels = mipAddress ? std::min(maxLevel + 1, fullChain) : 1;
                const uint32_t packedLevel = packedMips ? (log2Ceil(std::min(width, height)) > 4 ? log2Ceil(std::min(width, height)) - 4 : 0) : ~0u;
                auto nextPow2 = [&](uint32_t value) { return 1u << log2Ceil(value); };
                struct Level
                {
                    uint32_t width, height;      // texels
                    uint32_t blocksX, blocksY;
                    uint32_t address;             // dirección física de su memoria (o de la cola)
                    uint32_t pitchBlocks;         // de la memoria (bloques por fila)
                    uint32_t offsetX = 0, offsetY = 0; // en bloques, dentro de la cola
                    size_t sliceStride;           // entre caras
                    size_t size;                  // de una cara
                };
                std::vector<Level> levelInfo;
                size_t mipOffset = 0;
                for (uint32_t level = 0; level < levels; level++)
                {
                    Level info;
                    info.width = std::max(width >> level, 1u);
                    info.height = std::max(height >> level, 1u);
                    info.blocksX = (info.width + blockSize - 1) / blockSize;
                    info.blocksY = (info.height + blockSize - 1) / blockSize;
                    const bool inTail = level >= packedLevel;
                    const uint32_t storageLevel = inTail ? packedLevel : level; // el nivel cuya memoria se lee
                    uint32_t strideBlocksX, strideBlocksY;
                    if (storageLevel == 0)
                    {
                        strideBlocksX = pitchBlocks;
                        strideBlocksY = blocksY;
                    }
                    else
                    {
                        strideBlocksX = (std::max(nextPow2(width) >> storageLevel, 1u) + blockSize - 1) / blockSize;
                        strideBlocksY = (std::max(nextPow2(height) >> storageLevel, 1u) + blockSize - 1) / blockSize;
                    }
                    if (tiled)
                    {
                        strideBlocksX = (strideBlocksX + 31) & ~31u;
                        info.size = TiledSize(strideBlocksX, strideBlocksY, bytesPerBlock);
                    }
                    else
                    {
                        if (storageLevel != 0)
                            strideBlocksX = ((strideBlocksX * bytesPerBlock + 255) & ~255u) / bytesPerBlock;
                        info.size = size_t(strideBlocksX) * ((strideBlocksY + 31) & ~31u) * bytesPerBlock;
                    }
                    info.pitchBlocks = strideBlocksX;
                    info.sliceStride = (info.size + 4095) & ~size_t(4095);
                    if (inTail && level != packedLevel)
                        info.address = levelInfo[packedLevel].address;
                    else if (level == 0)
                        info.address = base & 0x1FFFFFFF;
                    else
                    {
                        info.address = (mipAddress & 0x1FFFFFFF) + uint32_t(mipOffset);
                        mipOffset += info.sliceStride * faces;
                    }
                    if (inTail) // desplazamiento dentro de la cola (GetPackedMipOffset de Xenia)
                    {
                        const uint32_t log2Width = log2Ceil(width), log2Height = log2Ceil(height);
                        const uint32_t packedMip = level - packedLevel;
                        uint32_t x = 0, y = 0;
                        if (packedMip < 3)
                            (log2Width > log2Height ? y : x) = 16 >> packedMip;
                        else
                        {
                            const uint32_t offset = (1u << ((log2Width > log2Height ? log2Width : log2Height) - packedLevel)) >> (packedMip - 2);
                            (log2Width > log2Height ? x : y) = offset;
                        }
                        info.offsetX = x / blockSize;
                        info.offsetY = y / blockSize;
                        info.size = levelInfo.empty() || level == packedLevel ? info.size : levelInfo[packedLevel].size;
                    }
                    levelInfo.push_back(info);
                }

                // ¿Ha cambiado? Se mira la memoria original (con mosaico, las filas van de 32 en 32).
                const auto* source = static_cast<const uint8_t*>(memory::Translate(0xA0000000 | (base & 0x1FFFFFFF)));
                const size_t faceSize = levelInfo[0].size;
                const size_t faceStride = levelInfo[0].sliceStride; // las caras van alineadas a 4 KB
                const size_t rawSize = std::min<size_t>(faceStride * (faces - 1) + faceSize, 0x20000000 - (base & 0x1FFFFFFF));
                const size_t mipSize = std::min<size_t>(mipOffset, 0x20000000 - (mipAddress & 0x1FFFFFFF));
                const auto* mipSource = static_cast<const uint8_t*>(memory::Translate(0xA0000000 | (mipAddress & 0x1FFFFFFF)));
                // ¿Es la imagen de un resolve? Entonces se usa su copia en la GPU, mientras la memoria no
                // haya cambiado desde el resolve (se comprueba una vez por época).
                if (format == 6 && tiled && !cube && levels == 1)
                    for (ResolvedSurface& surface : surfaces)
                        if (surface.base == (base & 0x1FFFFFFF) && surface.pitch == pitchBlocks && surface.height == height &&
                            surface.endian == endian && width <= surface.pitch)
                        {
                            if (surface.validEpoch == epoch)
                                return surface.view;
                            if (surface.hash == HashMemory(source, rawSize))
                            {
                                surface.validEpoch = epoch;
                                return surface.view;
                            }
                        }
                if (format == 6 && tiled && cube && levels == 1)
                    if (id<MTLTexture> view = CubeFromSurfaces(base & 0x1FFFFFFF, faceStride, pitchBlocks, width, endian))
                        return view;
                const uint64_t key = (uint64_t(base) << 32) ^ (uint64_t(format) << 26) ^ (uint64_t(width) << 13) ^ height ^ (uint64_t(cube) << 63) ^
                                     (uint64_t(mipAddress) * 0x9E3779B1ull) ^ (uint64_t(levels) << 58);
                Texture& cached = textures[key];
                if (cached.texture && cached.epoch == epoch)
                    return cached.texture;
                // Si la textura cae sobre un resolve aún no escrito en memoria (y no ha podido usar su
                // copia), se escribe ahora para leer lo que toca.
                if (WritebackOverlapping(base & 0x1FFFFFFF, rawSize) || (mipSize && WritebackOverlapping(mipAddress & 0x1FFFFFFF, mipSize)))
                    return TextureFor(fetch, cube);
                // Sin escrituras en su memoria desde que se leyó (páginas vigiladas): no hace falta mirarla.
                if (cached.texture && memory::Unchanged(base & 0x1FFFFFFF, uint32_t(rawSize)) &&
                    (!mipSize || memory::Unchanged(mipAddress & 0x1FFFFFFF, uint32_t(mipSize))))
                {
                    cached.epoch = epoch;
                    return cached.texture;
                }
                // Se vigila antes de leerla: una escritura posterior la marcará como cambiada.
                memory::Watch(base & 0x1FFFFFFF, uint32_t(rawSize));
                if (mipSize)
                    memory::Watch(mipAddress & 0x1FFFFFFF, uint32_t(mipSize));
                const uint64_t hash = HashMemory(source, rawSize) ^ (mipSize ? HashMemory(mipSource, mipSize) * 31 : 0);
                if (cached.texture && cached.hash == hash)
                {
                    cached.epoch = epoch;
                    return cached.texture;
                }

                // Textura nueva (no se reescribe la anterior: puede usarla un dibujo aún sin enviar).
                MTLTextureDescriptor* descriptor = cube ?
                    [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:pixelFormat size:width mipmapped:levels > 1] :
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixelFormat width:width height:height mipmapped:levels > 1];
                descriptor.mipmapLevelCount = levels;
                cached.texture = [device newTextureWithDescriptor:descriptor];

                // Lectura de cada nivel y cara (quitando el mosaico) e intercambio de bytes.
                std::vector<uint8_t> data;
                for (uint32_t level = 0; level < levels; level++)
                {
                    const Level& info = levelInfo[level];
                    const size_t imageBytes = size_t(info.blocksX) * info.blocksY * bytesPerBlock;
                    for (uint32_t face = 0; face < faces; face++)
                    {
                        data.assign(imageBytes, 0);
                        const size_t start = size_t(info.address) + face * info.sliceStride;
                        if (start + info.size > 0x20000000)
                            break; // se sale de la memoria
                        const auto* levelSource = static_cast<const uint8_t*>(memory::Translate(0xA0000000 | uint32_t(start)));
                        for (uint32_t by = 0; by < info.blocksY; by++)
                        {
                            for (uint32_t bx = 0; bx < info.blocksX; bx++)
                            {
                                const uint32_t x = bx + info.offsetX, y = by + info.offsetY;
                                const size_t offset = tiled ? TiledOffset(x, y, info.pitchBlocks, log2Bytes) : (size_t(y) * info.pitchBlocks + x) * bytesPerBlock;
                                if (offset + bytesPerBlock <= info.size)
                                    std::memcpy(&data[(size_t(by) * info.blocksX + bx) * bytesPerBlock], levelSource + offset, bytesPerBlock);
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
                        [cached.texture replaceRegion:MTLRegionMake2D(0, 0, info.width, info.height) mipmapLevel:level slice:face
                            withBytes:data.data() bytesPerRow:info.blocksX * bytesPerBlock bytesPerImage:imageBytes];
                    }
                }
                cached.hash = hash;
                cached.epoch = epoch;
                if (Trace())
                    std::fprintf(stderr, "[render] textura 0x%08X %ux%u formato %u, %u niveles%s%s%s\n", base, width, height, format, levels,
                        packedMips ? " (cola empaquetada)" : "", tiled ? " (mosaico)" : "", cube ? " (cubo)" : "");
                return cached.texture;
            }

            id<MTLSamplerState> SamplerFor(const uint32_t* fetch)
            {
                // clamp_x/y: bits 10-12 y 13-15 de la palabra 0; filtros mag/min: bits 19-20 y 21-22 de la palabra 3;
                // color del borde: bits 0-1 de la palabra 5 (0 = negro transparente, 1 = blanco).
                // Filtro entre mipmaps: bits 23-24 de la palabra 3; anisótropo: bits 25-27.
                const uint32_t key = ((fetch[0] >> 10) & 0x3F) | (((fetch[3] >> 19) & 0x1FF) << 6) | ((fetch[5] & 3) << 15);
                auto it = samplers.find(key);
                if (it != samplers.end())
                    return it->second;
                // Modos de Xenos: 0 repetir, 1 espejo, 2 último texel, 3 espejo una vez, 4 y 5 lo mismo hasta
                // medio texel del borde (aquí como 2 y 3), 6 color del borde, 7 espejo y borde (aquí como 3).
                auto address = [](uint32_t clamp) {
                    switch (clamp)
                    {
                    case 0: return MTLSamplerAddressModeRepeat;
                    case 1: return MTLSamplerAddressModeMirrorRepeat;
                    case 3: case 5: case 7: return MTLSamplerAddressModeMirrorClampToEdge;
                    case 6: return MTLSamplerAddressModeClampToBorderColor;
                    default: return MTLSamplerAddressModeClampToEdge;
                    }
                };
                MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
                descriptor.sAddressMode = address((fetch[0] >> 10) & 7);
                descriptor.tAddressMode = address((fetch[0] >> 13) & 7);
                // El mapa de sombras, p. ej., usa borde blanco: fuera de él no hay sombra.
                descriptor.borderColor = (fetch[5] & 3) == 1 ? MTLSamplerBorderColorOpaqueWhite : MTLSamplerBorderColorTransparentBlack;
                descriptor.magFilter = ((fetch[3] >> 19) & 3) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
                descriptor.minFilter = ((fetch[3] >> 21) & 3) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
                // 0 = el más cercano, 1 = lineal, 2 = solo la base, 3 = lo que diga la instrucción (aquí lineal).
                const uint32_t mipFilter = (fetch[3] >> 23) & 3;
                descriptor.mipFilter = mipFilter == 0 ? MTLSamplerMipFilterNearest :
                                       mipFilter == 2 ? MTLSamplerMipFilterNotMipmapped : MTLSamplerMipFilterLinear;
                // Anisótropo: 0 = no, 1-5 = hasta 1, 2, 4, 8 y 16 muestras (6 y 7: de la instrucción; aquí 1).
                const uint32_t aniso = (fetch[3] >> 25) & 7;
                if (aniso >= 2 && aniso <= 5 && descriptor.minFilter == MTLSamplerMinMagFilterLinear)
                    descriptor.maxAnisotropy = 1u << (aniso - 1);
                id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
                samplers[key] = sampler;
                return sampler;
            }

            void BeginPass(uint32_t setIndex)
            {
                if (encoder && passSet == setIndex)
                    return;
                EndPass(false);
                passSet = setIndex;
                RenderSet& set = sets[setIndex];
                commands = [queue commandBuffer];
                MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
                const bool msaa = set.samples > 1;
                pass.colorAttachments[0].texture = msaa ? set.colorMS : set.color;
                pass.colorAttachments[0].loadAction = set.clearPending ? MTLLoadActionClear : MTLLoadActionLoad;
                pass.colorAttachments[0].clearColor = set.clearColor;
                pass.depthAttachment.texture = msaa ? set.depthMS : set.depth;
                pass.depthAttachment.loadAction = set.depthClearPending ? MTLLoadActionClear : MTLLoadActionLoad;
                pass.depthAttachment.clearDepth = set.clearDepth;
                pass.stencilAttachment.texture = msaa ? set.depthMS : set.depth;
                pass.stencilAttachment.loadAction = set.depthClearPending ? MTLLoadActionClear : MTLLoadActionLoad;
                pass.stencilAttachment.clearStencil = set.clearStencil;
                pass.stencilAttachment.storeAction = MTLStoreActionStore;
                if (msaa)
                {
                    // Se guardan las muestras (la pasada siguiente sigue encima) y se resuelve (muestra 0
                    // para la profundidad) en las texturas de una muestra.
                    pass.colorAttachments[0].resolveTexture = set.color;
                    pass.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
                    pass.depthAttachment.resolveTexture = set.depth;
                    pass.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
                    pass.depthAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
                }
                else
                {
                    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                    pass.depthAttachment.storeAction = MTLStoreActionStore;
                }
                set.clearPending = false;
                set.depthClearPending = false;
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
                    lastCommit = commands;
                    if (wait)
                        [commands waitUntilCompleted];
                    commands = nil;
                }
            }

            bool Draw(const DrawCall& draw)
            {
                // Puntos, listas de triángulos, abanicos, tiras, listas de rectángulos y de quads.
                const uint32_t primitive = draw.primitive;
                if ((primitive != 1 && primitive != 4 && primitive != 5 && primitive != 6 && primitive != 8 && primitive != 13) ||
                    draw.count == 0 || (primitive == 5 && draw.count < 3) ||
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

                const ConstantOffsets constants = UploadConstants(draw);
                const uint32_t setIndex = SetFor(draw.registers);
                BeginPass(setIndex);
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
                state.targetSize[0] = sets[setIndex].width;
                state.targetSize[1] = sets[setIndex].height;
                int32_t windowX, windowY;
                WindowOffset(draw.registers, windowX, windowY);
                state.windowOffset[0] = float(windowX);
                state.windowOffset[1] = float(windowY);
                std::memcpy(state.bools, &draw.registers[kRegBoolConstants], sizeof(state.bools));
                state.alphaTest = draw.registers[kRegColorControl] & 0x1F;
                if (sets[setIndex].samples > 1)
                    state.alphaTest &= ~0x10u; // alpha-to-mask: lo hace la cobertura por alfa del pipeline
                state.alphaRef = AsFloat(draw.registers[kRegAlphaRef]);
                // Tamaños de punto en 12.4 de coma fija, como radio (Xenia: diámetro = valor * 2 / 16).
                const uint32_t pointSize = draw.registers[kRegPointSize], pointMinMax = draw.registers[kRegPointMinMax];
                state.pointSize[0] = (pointSize >> 16) / 8.0f;
                state.pointSize[1] = (pointSize & 0xFFFF) / 8.0f;
                state.pointMinMax[0] = (pointMinMax & 0xFFFF) / 8.0f;
                state.pointMinMax[1] = (pointMinMax >> 16) / 8.0f;

                const uint32_t* fetchConstants = &draw.registers[kRegFetchConstants];
                [encoder setVertexBuffer:memory offset:0 atIndex:0];
                [encoder setVertexBuffer:constantRing offset:constants.vertex atIndex:1];
                [encoder setVertexBuffer:constantRing offset:constants.fetch atIndex:2];
                [encoder setVertexBytes:&state length:sizeof(state) atIndex:3];
                [encoder setFragmentBuffer:memory offset:0 atIndex:0];
                [encoder setFragmentBuffer:constantRing offset:constants.pixel atIndex:1];
                [encoder setFragmentBuffer:constantRing offset:constants.fetch atIndex:2];
                [encoder setFragmentBytes:&state length:sizeof(state) atIndex:3];
                for (uint32_t entry : textureSlots)
                {
                    const uint32_t slot = entry & 31;
                    const bool cube = entry & kCubeSlot;
                    const uint32_t* fetch = &fetchConstants[slot * 6];
                    SlotMemo& memo = slotMemos[slot];
                    if (memo.epoch != epoch || memo.cube != cube || std::memcmp(memo.fetch, fetch, sizeof(memo.fetch)) != 0)
                    {
                        memo.texture = TextureFor(fetch, cube);
                        memo.sampler = SamplerFor(fetch);
                        std::memcpy(memo.fetch, fetch, sizeof(memo.fetch));
                        memo.epoch = epoch;
                        memo.cube = cube;
                    }
                    static const bool traceDepth = Trace();
                    if (traceDepth)
                    {
                        // Qué shaders de píxeles leen una profundidad (resolve de fuente 4) y en qué ranura.
                        static std::unordered_set<uint64_t> reported;
                        const uint32_t format = fetch[1] & 0x3F;
                        if ((format == 22 || format == 23) && reported.insert(draw.pixelShader->hash * 32 + slot).second)
                            std::fprintf(stderr, "[render] ps_%016llx lee profundidad en t%u (0x%08X, %s) fetch %08X %08X %08X %08X %08X %08X\n",
                                (unsigned long long)draw.pixelShader->hash, slot, fetch[1] & 0xFFFFF000,
                                memo.texture == farDepthTexture ? "sin resolve" : "resolve",
                                fetch[0], fetch[1], fetch[2], fetch[3], fetch[4], fetch[5]);
                    }
                    [encoder setFragmentTexture:memo.texture atIndex:slot];
                    [encoder setFragmentSamplerState:memo.sampler atIndex:slot];
                }

                const uint32_t vertices = primitive == 13 ? draw.count / 4 * 6 : primitive == 8 ? draw.count / 3 * 6 :
                                          primitive == 5 ? (draw.count - 2) * 3 : primitive == 1 ? draw.count * 6 : draw.count;
                [encoder drawPrimitives:primitive == 6 ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle
                    vertexStart:0 vertexCount:vertices];
                draws++;
                return true;
            }

            // Lee de una textura a la escala de resolución el rectángulo que corresponde a uno del juego
            // (left, top, width, height). El píxel (x, y) del juego es el (Scaled(x), Scaled(y)) leído.
            void ReadScaled(id<MTLTexture> texture, uint32_t left, uint32_t top, uint32_t width, uint32_t height,
                std::vector<uint32_t>& pixels, uint32_t& scaledLeft, uint32_t& scaledTop, uint32_t& scaledWidth)
            {
                scaledLeft = Scaled(left);
                scaledTop = Scaled(top);
                scaledWidth = std::min<uint32_t>(Scaled(left + width - 1) + 1, uint32_t(texture.width)) - scaledLeft;
                const uint32_t scaledHeight = std::min<uint32_t>(Scaled(top + height - 1) + 1, uint32_t(texture.height)) - scaledTop;
                pixels.assign(size_t(scaledWidth) * scaledHeight, 0);
                [texture getBytes:pixels.data() bytesPerRow:scaledWidth * 4
                    fromRegion:MTLRegionMake2D(scaledLeft, scaledTop, scaledWidth, scaledHeight) mipmapLevel:0];
            }

            // Escribe en la memoria del juego (con mosaico y su orden de bytes) lo que un resolve
            // diferido dejó solo en la GPU.
            void WritebackSurface(ResolvedSurface& surface)
            {
                if (!surface.pending)
                    return;
                surface.pending = false;
                EndPass(false);
                [lastCommit waitUntilCompleted];
                const uint32_t left = surface.pendingLeft, top = surface.pendingTop;
                const uint32_t width = surface.pendingRight - left, height = surface.pendingBottom - top;
                std::vector<uint32_t> pixels;
                uint32_t scaledLeft, scaledTop, scaledWidth;
                ReadScaled(surface.texture, left, top, width, height, pixels, scaledLeft, scaledTop, scaledWidth);
                auto* dest = static_cast<uint8_t*>(memory::Translate(0xA0000000 | surface.base));
                for (uint32_t y = 0; y < height; y++)
                    for (uint32_t x = 0; x < width; x++)
                    {
                        const uint32_t rgba = pixels[size_t(Scaled(top + y) - scaledTop) * scaledWidth + Scaled(left + x) - scaledLeft];
                        uint32_t value = (rgba & 0xFF00FF00) | ((rgba & 0xFF) << 16) | ((rgba >> 16) & 0xFF);
                        switch (surface.endian)
                        {
                        case 1: value = ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF); break;
                        case 2: value = __builtin_bswap32(value); break;
                        case 3: value = (value << 16) | (value >> 16); break;
                        default: break;
                        }
                        std::memcpy(dest + TiledOffset(left + x, top + y, surface.pitch, 2), &value, 4);
                    }
                surface.hash = HashMemory(dest, std::min<size_t>(TiledSize(surface.pitch, surface.height, 4), 0x20000000 - surface.base));
                epoch++; // la memoria del juego ha cambiado
                surface.validEpoch = epoch;
            }

            // Escribe los resolves pendientes que se solapan con [base, base + size). Devuelve si escribió alguno.
            bool WritebackOverlapping(uint32_t base, size_t size)
            {
                bool wrote = false;
                for (ResolvedSurface& surface : surfaces)
                {
                    const size_t surfaceSize = TiledSize(surface.pitch, surface.height, 4);
                    if (surface.pending && base < surface.base + surfaceSize && surface.base < base + size)
                    {
                        WritebackSurface(surface);
                        wrote = true;
                    }
                }
                return wrote;
            }

            // El front buffer para la ventana, directamente de la copia en la GPU (BGRA, sin mosaico,
            // a la escala de resolución). Después se aplica un cambio de escala pedido.
            bool ReadFrame(uint32_t base, uint32_t width, uint32_t height, Frame& frame)
            {
                epoch++;
                bool found = false;
                for (ResolvedSurface& surface : surfaces)
                {
                    if (surface.base != base || surface.pitch != width || surface.height < height)
                        continue;
                    EndPass(false);
                    [lastCommit waitUntilCompleted];
                    frame.width = std::min<uint32_t>(Scaled(width), uint32_t(surface.texture.width));
                    frame.height = std::min<uint32_t>(Scaled(height), uint32_t(surface.texture.height));
                    frame.pixels.resize(size_t(frame.width) * frame.height);
                    [surface.texture getBytes:frame.pixels.data() bytesPerRow:frame.width * 4
                        fromRegion:MTLRegionMake2D(0, 0, frame.width, frame.height) mipmapLevel:0];
                    for (uint32_t& pixel : frame.pixels)
                    {
                        const uint32_t rgba = pixel;
                        pixel = (rgba & 0x0000FF00) | ((rgba & 0xFF) << 16) | ((rgba >> 16) & 0xFF) | 0xFF000000u;
                    }
                    found = true;
                    break;
                }
                ApplyScale();
                return found;
            }

            // Copia en la GPU de lo que acaba de escribir un resolve (ver ResolvedSurface).
            void UpdateSurface(id<MTLTexture> source, uint32_t base, uint32_t pitch, uint32_t height, uint32_t endian,
                uint32_t left, uint32_t top, uint32_t width, uint32_t rows, bool deferred)
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
                    {
                        WritebackSurface(*surface);
                        surfaces.erase(surfaces.begin() + (surface - surfaces.data()));
                    }
                    if (surfaces.size() >= 32)
                    {
                        WritebackSurface(surfaces.front());
                        surfaces.erase(surfaces.begin());
                    }
                    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                        width:Scaled(pitch) height:Scaled(height) mipmapped:NO];
                    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
                    descriptor.storageMode = MTLStorageModeShared; // se lee al escribir la memoria o presentar
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
                surface->version = ++surfaceVersions; // único entre todas: una imagen nueva no repite número

                // A la escala: el tamaño sale del destino (así las franjas seguidas no dejan huecos),
                // recortado a las dos texturas.
                const uint32_t sourceX = Scaled(left), sourceY = Scaled(top);
                const uint32_t destY = Scaled(offsetY + top);
                const uint32_t copyWidth = std::min({ Scaled(left + width) - sourceX, uint32_t(source.width) - sourceX,
                    uint32_t(surface->texture.width) - sourceX });
                const uint32_t copyRows = std::min({ Scaled(offsetY + top + rows) - destY, uint32_t(source.height) - sourceY,
                    uint32_t(surface->texture.height) - destY });
                id<MTLCommandBuffer> copy = [queue commandBuffer];
                id<MTLBlitCommandEncoder> blit = [copy blitCommandEncoder];
                [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(sourceX, sourceY, 0)
                    sourceSize:MTLSizeMake(copyWidth, copyRows, 1) toTexture:surface->texture destinationSlice:0 destinationLevel:0
                    destinationOrigin:MTLOriginMake(sourceX, destY, 0)];
                [blit endEncoding];
                [copy commit];
                lastCommit = copy;

                if (deferred)
                {
                    const uint32_t newTop = offsetY + top, newBottom = offsetY + top + rows;
                    if (!surface->pending)
                    {
                        surface->pendingLeft = left; surface->pendingTop = newTop;
                        surface->pendingRight = left + width; surface->pendingBottom = newBottom;
                    }
                    else
                    {
                        surface->pendingLeft = std::min(surface->pendingLeft, left);
                        surface->pendingTop = std::min(surface->pendingTop, newTop);
                        surface->pendingRight = std::max(surface->pendingRight, left + width);
                        surface->pendingBottom = std::max(surface->pendingBottom, newBottom);
                    }
                    surface->pending = true;
                }
                // Huella de la memoria tal como queda tras el resolve (una vez por época si es diferido:
                // entonces el resolve no la cambia).
                if (!deferred || surface->validEpoch != epoch)
                {
                    const auto* memoryStart = static_cast<const uint8_t*>(memory::Translate(0xA0000000 | surface->base));
                    surface->hash = HashMemory(memoryStart, std::min<size_t>(TiledSize(pitch, surface->height, 4),
                        0x20000000 - surface->base));
                    surface->validEpoch = epoch;
                }
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

            // Copia la profundidad (en la GPU) a la DepthSurface de su dirección. El rectángulo es el de
            // los vértices recortado por PA_SC_WINDOW_SCISSOR, y con el desplazamiento de ventana pasa a
            // la EDRAM y a la franja de destino (cuya base ya apunta a ella).
            void ResolveDepth(const uint32_t* registers, uint32_t base, uint32_t pitch, uint32_t height)
            {
                const RenderSet& set = sets[SetFor(registers)];
                const uint32_t vertices = 0xA0000000 | (registers[kRegFetchConstants] & 0x1FFFFFFC);
                float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
                for (int i = 0; i < 3; i++)
                {
                    const float x = AsFloat(__builtin_bswap32(*static_cast<const uint32_t*>(memory::Translate(vertices + i * 8))));
                    const float y = AsFloat(__builtin_bswap32(*static_cast<const uint32_t*>(memory::Translate(vertices + i * 8 + 4))));
                    minX = std::min(minX, x); maxX = std::max(maxX, x);
                    minY = std::min(minY, y); maxY = std::max(maxY, y);
                }
                const uint32_t scissorTL = registers[0x2081], scissorBR = registers[0x2082];
                minX = std::max(minX, float(scissorTL & 0x7FFF)); minY = std::max(minY, float((scissorTL >> 16) & 0x7FFF));
                maxX = std::min(maxX, float(scissorBR & 0x7FFF)); maxY = std::min(maxY, float((scissorBR >> 16) & 0x7FFF));
                int32_t windowX, windowY;
                WindowOffset(registers, windowX, windowY);
                const int32_t left = std::max(int32_t(minX) + windowX, 0), top = std::max(int32_t(minY) + windowY, 0);
                const int32_t right = std::min({ int32_t(maxX) + windowX, int32_t(pitch), int32_t(set.width) });
                const int32_t bottom = std::min({ int32_t(maxY) + windowY, int32_t(height), int32_t(set.height) });
                if (right <= left || bottom <= top || pitch == 0 || height == 0)
                    return;

                // ¿Una franja de una imagen ya conocida?
                DepthSurface* surface = nullptr;
                uint32_t offsetY = 0;
                for (DepthSurface& candidate : depthSurfaces)
                    if (candidate.pitch == pitch && base >= candidate.base && base < candidate.base + candidate.pitch * 4 * candidate.height &&
                        (base - candidate.base) % (pitch * 4) == 0)
                    {
                        surface = &candidate;
                        offsetY = (base - candidate.base) / (pitch * 4);
                        break;
                    }
                if (!surface || offsetY + bottom > surface->height)
                {
                    if (surface)
                        depthSurfaces.erase(depthSurfaces.begin() + (surface - depthSurfaces.data()));
                    if (depthSurfaces.size() >= 8)
                        depthSurfaces.erase(depthSurfaces.begin());
                    DepthSurface created;
                    created.base = base;
                    created.pitch = pitch;
                    created.height = height;
                    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                        width:Scaled(std::min(pitch, kTargetWidth)) height:Scaled(std::min(height, kTargetHeight)) mipmapped:NO];
                    descriptor.usage = MTLTextureUsageShaderRead;
                    descriptor.storageMode = MTLStorageModePrivate;
                    created.texture = [device newTextureWithDescriptor:descriptor];
                    depthSurfaces.push_back(created);
                    surface = &depthSurfaces.back();
                    offsetY = 0;
                }
                if (Scaled(offsetY + bottom) > surface->texture.height)
                    return;

                EndPass(false);
                // A la escala de resolución, como en UpdateSurface.
                const uint32_t sourceX = Scaled(left), sourceY = Scaled(top), destY = Scaled(offsetY + top);
                const uint32_t width = std::min({ Scaled(right) - sourceX, uint32_t(set.depth.width) - sourceX, uint32_t(surface->texture.width) - sourceX });
                const uint32_t rows = std::min(Scaled(offsetY + bottom) - destY, uint32_t(set.depth.height) - sourceY);
                id<MTLCommandBuffer> copy = [queue commandBuffer];
                id<MTLBlitCommandEncoder> blit = [copy blitCommandEncoder];
                [blit copyFromTexture:set.depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(sourceX, sourceY, 0)
                    sourceSize:MTLSizeMake(width, rows, 1) toBuffer:depthStaging destinationOffset:0
                    destinationBytesPerRow:width * 4 destinationBytesPerImage:size_t(width) * rows * 4
                    options:MTLBlitOptionDepthFromDepthStencil];
                [blit copyFromBuffer:depthStaging sourceOffset:0 sourceBytesPerRow:width * 4 sourceBytesPerImage:size_t(width) * rows * 4
                    sourceSize:MTLSizeMake(width, rows, 1) toTexture:surface->texture destinationSlice:0 destinationLevel:0
                    destinationOrigin:MTLOriginMake(sourceX, destY, 0)];
                [blit endEncoding];
                [copy commit];
                lastCommit = copy;
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
                RenderSet& set = sets[SetFor(registers)];
                uint32_t left = 0, top = 0, right = destPitch, bottom = destHeight;
                ResolveRect(registers, left, top, right, bottom);
                right = std::min({right, destPitch, set.width});
                bottom = std::min({bottom, destHeight, set.height});
                if (immediateWriteback)
                    epoch++; // la memoria cambia en el acto
                if (source == 4 && destBase)
                    ResolveDepth(registers, destBase & 0x1FFFFFFF, destPitch, destHeight);
                else if (source < 4 && destFormat == 6 && destBase && right > left && bottom > top && !immediateWriteback)
                {
                    // Solo en la GPU: la memoria se escribe cuando alguien la necesita (WritebackSurface).
                    EndPass(false);
                    UpdateSurface(set.color, destBase & 0x1FFFFFFF, destPitch, destHeight, destEndian, left, top, right - left, bottom - top, true);
                }
                else if (source < 4 && destFormat == 6 && destBase && right > left && bottom > top)
                {
                    EndPass(true);
                    const uint32_t width = right - left, height = bottom - top;
                    std::vector<uint32_t> pixels;
                    uint32_t scaledLeft, scaledTop, scaledWidth;
                    ReadScaled(set.color, left, top, width, height, pixels, scaledLeft, scaledTop, scaledWidth);
                    auto* dest = static_cast<uint8_t*>(memory::Translate(0xA0000000 | (destBase & 0x1FFFFFFF)));
                    for (uint32_t y = 0; y < height; y++)
                    {
                        for (uint32_t x = 0; x < width; x++)
                        {
                            // RGBA8 de Metal (R en el byte bajo) -> palabra 0xAARRGGBB en memoria.
                            const uint32_t rgba = pixels[size_t(Scaled(top + y) - scaledTop) * scaledWidth + Scaled(left + x) - scaledLeft];
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
                    UpdateSurface(set.color, destBase & 0x1FFFFFFF, destPitch, destHeight, destEndian, left, top, width, height, false);
                }

                // Borrado tras la copia (RB_COPY_CONTROL bit 8): en la siguiente pasada.
                if (control & 0x100)
                {
                    EndPass(false);
                    const uint32_t clear = registers[kRegColorClear];
                    set.clearColor = MTLClearColorMake(((clear >> 16) & 0xFF) / 255.0, ((clear >> 8) & 0xFF) / 255.0,
                        (clear & 0xFF) / 255.0, ((clear >> 24) & 0xFF) / 255.0);
                    set.clearPending = true;
                }
                // Borrado de profundidad / stencil (bit 9). D24S8 o D24FS8 (20e4) según RB_DEPTH_INFO.
                if (control & 0x200)
                {
                    EndPass(false);
                    const uint32_t clear = registers[kRegDepthClear], value = clear >> 8;
                    if (registers[kRegDepthInfo] & 0x10000)
                    {
                        const uint32_t exponent = value >> 20, mantissa = value & 0xFFFFF;
                        set.clearDepth = exponent ? std::ldexp(1.0 + mantissa / 1048576.0, int(exponent) - 15) : std::ldexp(mantissa / 1048576.0, -14);
                        set.clearDepth = std::min(set.clearDepth, 1.0);
                    }
                    else
                    {
                        set.clearDepth = value / 16777215.0;
                    }
                    set.clearStencil = uint8_t(clear & 0xFF);
                    set.depthClearPending = true;
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

    bool RendererReadFrame(uint32_t physical, uint32_t width, uint32_t height, Frame& frame)
    {
        @autoreleasepool
        {
            Renderer* renderer = Get();
            return renderer && renderer->ReadFrame(physical & 0x1FFFFFFF, width, height, frame);
        }
    }

    void SetResolutionScale(float scale)
    {
        gRequestedScale = std::clamp(scale, 1.0f, 2.0f);
    }

    float ResolutionScale()
    {
        return gRequestedScale.load();
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
