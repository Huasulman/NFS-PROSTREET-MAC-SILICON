// XMA: el decodificador de audio por hardware. Cada contexto son 64 bytes en memoria física
// (XMA_CONTEXT_DATA, 16 palabras big-endian con campos de bits, según Xenia) que describen los
// búferes de entrada (paquetes XMA) y el de salida (PCM) de una voz.
//
// El decodificador es un hilo que, como el hardware, recorre los contextos activos: toma los
// paquetes XMA (2 KB) del búfer de entrada actual, los decodifica con FFmpeg y escribe el PCM
// (16 bits big-endian) en el anillo de salida. Al acabar un búfer de entrada lo marca como no
// válido y pasa al otro, y el juego lo va rellenando: así llegan la música y los sonidos de menú.
//
// PROSTREET_XMA_DUMP=<carpeta>: guarda el PCM decodificado de cada contexto en
// <carpeta>/xma_<contexto>_<canales>ch.raw (16 bits little-endian, 48 kHz si no se indica otra).

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "imports.h"
#include "../memory.h"

namespace
{
    constexpr uint32_t kRegisterBase = 0x7FEA0000;            // registros del decodificador (MMIO)
    constexpr uint32_t kRegContextArrayAddress = 0x1800;
    constexpr uint32_t kContextSize = 64;
    constexpr uint32_t kContextCount = 320; // como el hardware

    std::mutex gLock;
    uint32_t gContexts = 0; // bloque de kContextCount contextos en memoria física
    std::vector<bool> gUsed(kContextCount);

    // Un campo de bits de la palabra `word` del contexto (bit 0 = el menos significativo).
    struct Field
    {
        uint32_t word, shift, bits;
    };

    // Palabra 0
    constexpr Field kInput0PacketCount{0, 0, 12};
    constexpr Field kLoopCount{0, 12, 8};
    constexpr Field kInput0Valid{0, 20, 1};
    constexpr Field kInput1Valid{0, 21, 1};
    constexpr Field kOutputBlockCount{0, 22, 5};
    constexpr Field kOutputWriteOffset{0, 27, 5};
    // Palabra 1
    constexpr Field kInput1PacketCount{1, 0, 12};
    constexpr Field kLoopSubframeEnd{1, 14, 3};
    constexpr Field kLoopSubframeSkip{1, 17, 3};
    constexpr Field kSubframeDecodeCount{1, 20, 4};
    constexpr Field kSampleRate{1, 27, 2};
    constexpr Field kStereo{1, 29, 1};
    constexpr Field kOutputValid{1, 31, 1};
    // Palabras 2 a 9
    constexpr Field kInputReadOffset{2, 0, 26};
    constexpr Field kLoopStart{3, 0, 26};
    constexpr Field kLoopEnd{4, 0, 26};
    constexpr Field kInput0Pointer{5, 0, 32};
    constexpr Field kInput1Pointer{6, 0, 32};
    constexpr Field kOutputPointer{7, 0, 32};
    constexpr Field kWorkPointer{8, 0, 32};
    constexpr Field kOutputReadOffset{9, 0, 5};
    constexpr Field kCurrentBuffer{4, 31, 1};

    constexpr uint32_t kPacketBytes = 2048, kBlockBytes = 256;
    constexpr uint32_t kPacketHeaderBits = 32;

    uint32_t Mask(const Field& field)
    {
        return field.bits == 32 ? 0xFFFFFFFFu : ((1u << field.bits) - 1) << field.shift;
    }

    uint32_t Get(uint32_t context, const Field& field)
    {
        return (memory::Read32(context + field.word * 4) & Mask(field)) >> field.shift;
    }

    void Set(uint32_t context, const Field& field, uint32_t value)
    {
        const uint32_t address = context + field.word * 4;
        const uint32_t mask = Mask(field);
        memory::Write32(address, (memory::Read32(address) & ~mask) | ((value << field.shift) & mask));
    }

    uint8_t* Physical(uint32_t address)
    {
        return static_cast<uint8_t*>(memory::Translate(0xA0000000 | (address & 0x1FFFFFFF)));
    }

    // Estado del anfitrión de cada contexto: el decodificador de FFmpeg y el PCM que aún no ha
    // cabido en la salida (ya en 16 bits big-endian).
    struct Decoder
    {
        bool enabled = false;
        bool kicked = false; // activado y aún sin su pasada de Work
        AVCodecContext* codec = nullptr;
        AVPacket* packet = nullptr;
        AVFrame* frame = nullptr;
        std::vector<uint8_t> pending;
        bool failed = false;
        uint32_t packets = 0, samples = 0; // para la traza
        float peak = 0.0f;
        uint32_t consumed = 0, kicks = 0; // avances del puntero de lectura de salida, activaciones
        FILE* dump = nullptr; // PROSTREET_XMA_DUMP: PCM decodificado (16 bits little-endian)
    };
    Decoder gDecoders[kContextCount];

    uint32_t IndexOf(uint32_t context)
    {
        return (context - gContexts) / kContextSize;
    }

    void Reset(Decoder& decoder)
    {
        avcodec_free_context(&decoder.codec);
        decoder.pending.clear();
        decoder.failed = false;
        decoder.packets = decoder.samples = 0;
        decoder.peak = 0.0f;
        decoder.consumed = decoder.kicks = 0;
    }

    bool Open(Decoder& decoder, uint32_t context)
    {
        static const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
        if (!codec)
            return false;
        static constexpr int kRates[] = { 24000, 32000, 44100, 48000 };
        const int channels = Get(context, kStereo) ? 2 : 1;
        decoder.codec = avcodec_alloc_context3(codec);
        decoder.codec->sample_rate = kRates[Get(context, kSampleRate)];
        av_channel_layout_default(&decoder.codec->ch_layout, channels);
        decoder.codec->block_align = kPacketBytes;
        // XMA2WAVEFORMATEX (34 bytes): número de flujos y máscara de canales.
        decoder.codec->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
        decoder.codec->extradata_size = 34;
        decoder.codec->extradata[0] = 1;
        const uint32_t mask = channels == 2 ? 3 : 4; // estéreo / centro
        std::memcpy(decoder.codec->extradata + 2, &mask, 4); // little-endian
        if (avcodec_open2(decoder.codec, codec, nullptr) < 0)
        {
            std::fprintf(stderr, "[xma] FFmpeg no pudo abrir el decodificador XMA2\n");
            avcodec_free_context(&decoder.codec);
            return false;
        }
        if (!decoder.packet)
            decoder.packet = av_packet_alloc();
        if (!decoder.frame)
            decoder.frame = av_frame_alloc();
        return true;
    }

    // Pasa a `pending` (16 bits big-endian, entrelazado) los frames que FFmpeg tenga listos.
    void ReceiveFrames(Decoder& decoder)
    {
        while (avcodec_receive_frame(decoder.codec, decoder.frame) == 0)
        {
            const int channels = decoder.frame->ch_layout.nb_channels, samples = decoder.frame->nb_samples;
            decoder.samples += uint32_t(samples);
            const size_t start = decoder.pending.size();
            decoder.pending.resize(start + size_t(samples) * channels * 2);
            uint8_t* out = decoder.pending.data() + start;
            for (int i = 0; i < samples; i++)
                for (int c = 0; c < channels; c++)
                {
                    const float value = reinterpret_cast<const float*>(decoder.frame->extended_data[c])[i];
                    decoder.peak = std::max(decoder.peak, std::fabs(value));
                    const int16_t sample = int16_t(std::clamp(value, -1.0f, 1.0f) * 32767.0f);
                    *out++ = uint8_t(uint16_t(sample) >> 8);
                    *out++ = uint8_t(sample);
                    if (decoder.dump)
                        std::fwrite(&sample, 2, 1, decoder.dump);
                }
            av_frame_unref(decoder.frame);
        }
    }

    // FFmpeg retiene los frames del último paquete hasta ver el siguiente; al acabarse la entrada
    // (fin de un sonido) se le piden y se reinicia. Sin esto, los sonidos de un solo paquete (como
    // el de "atrás" en los menús) no producen nada.
    void Drain(Decoder& decoder)
    {
        if (!decoder.codec || avcodec_send_packet(decoder.codec, nullptr) < 0)
            return;
        ReceiveFrames(decoder);
        avcodec_flush_buffers(decoder.codec);
    }

    // Decodifica un paquete y deja el PCM en `pending`.
    void DecodePacket(Decoder& decoder, const uint8_t* data)
    {
        if (av_new_packet(decoder.packet, kPacketBytes) < 0)
            return;
        std::memcpy(decoder.packet->data, data, kPacketBytes);
        int sent = avcodec_send_packet(decoder.codec, decoder.packet);
        if (sent < 0)
        {
            // Pasa si el paquete no continúa al anterior (p. ej. tras vaciar el decodificador al
            // final de un sonido, o un contexto reutilizado): se empieza de cero en este paquete.
            avcodec_flush_buffers(decoder.codec);
            sent = avcodec_send_packet(decoder.codec, decoder.packet);
        }
        av_packet_unref(decoder.packet);
        if (sent < 0)
        {
            static int reported = 0;
            if (reported++ < 10)
                std::fprintf(stderr, "[xma] paquete rechazado por FFmpeg (%d): se salta\n", sent);
            return;
        }
        decoder.packets++;
        static const char* dump = std::getenv("PROSTREET_XMA_DUMP"); // carpeta
        if (dump && !decoder.dump)
        {
            char name[512];
            std::snprintf(name, sizeof(name), "%s/xma_%03d_%dch.raw", dump, int(&decoder - gDecoders),
                decoder.codec->ch_layout.nb_channels);
            decoder.dump = std::fopen(name, "ab");
        }
        ReceiveFrames(decoder);
    }

    // Lo que hace el hardware cada vez que se activa un contexto (con gLock tomado), como Xenia:
    // llena la salida con frames enteros mientras haya entrada; el anillo puede llenarse del todo
    // (escritura == lectura) y entonces, o si se acaba la entrada, borra output_buffer_valid, que
    // es la señal que espera el juego para recoger el PCM y darle otro hueco.
    void Work(uint32_t context, Decoder& decoder)
    {
        if (!Get(context, kInput0Valid) && !Get(context, kInput1Valid))
        {
            Set(context, kOutputValid, 0);
            return;
        }
        if (!Get(context, kOutputValid))
            return;
        if (!decoder.codec && (decoder.failed || !Open(decoder, context)))
        {
            decoder.failed = true;
            return;
        }
        const uint32_t capacity = Get(context, kOutputBlockCount) * kBlockBytes;
        if (capacity == 0)
            return;
        uint8_t* output = Physical(Get(context, kOutputPointer));
        uint32_t write = Get(context, kOutputWriteOffset) * kBlockBytes;
        const uint32_t read = Get(context, kOutputReadOffset) * kBlockBytes;
        uint32_t room = read == write ? capacity : (write < read ? read - write : capacity - write + read);
        const uint32_t frameBytes = 512 * 2 * uint32_t(decoder.codec->ch_layout.nb_channels);
        room -= room % frameBytes;

        while (room >= frameBytes)
        {
            // Un frame de PCM: lo pendiente o, si no llega, el siguiente paquete de entrada.
            while (decoder.pending.size() < frameBytes)
            {
                const uint32_t current = Get(context, kCurrentBuffer);
                const Field& valid = current ? kInput1Valid : kInput0Valid;
                if (!Get(context, valid))
                    break;
                const uint32_t packets = Get(context, current ? kInput1PacketCount : kInput0PacketCount);
                const uint32_t packet = Get(context, kInputReadOffset) / (kPacketBytes * 8);
                if (packet < packets)
                    DecodePacket(decoder, Physical(Get(context, current ? kInput1Pointer : kInput0Pointer)) + packet * kPacketBytes);
                if (packet + 1 < packets)
                {
                    Set(context, kInputReadOffset, (packet + 1) * kPacketBytes * 8 + kPacketHeaderBits);
                }
                else
                {
                    // Búfer agotado: se devuelve al juego y se pasa al otro.
                    Set(context, valid, 0);
                    Set(context, kCurrentBuffer, current ^ 1);
                    Set(context, kInputReadOffset, kPacketHeaderBits);
                    if (!Get(context, current ? kInput0Valid : kInput1Valid))
                        Drain(decoder);
                }
            }
            if (decoder.pending.size() < frameBytes)
                break; // sin más entrada: el juego dará más y volverá a activar el contexto
            for (uint32_t done = 0; done < frameBytes;)
            {
                const uint32_t chunk = std::min(frameBytes - done, capacity - write);
                std::memcpy(output + write, decoder.pending.data() + done, chunk);
                done += chunk;
                write = (write + chunk) % capacity;
            }
            decoder.pending.erase(decoder.pending.begin(), decoder.pending.begin() + frameBytes);
            room -= frameBytes;
        }
        Set(context, kOutputWriteOffset, write / kBlockBytes);
        if (write == read)
            Set(context, kOutputValid, 0);
    }

    // Cada activación (XMAEnableContext) pide una pasada de Work, como un "kick" del hardware.
    void DecoderThread()
    {
        for (;;)
        {
            {
                std::lock_guard lock(gLock);
                for (uint32_t index = 0; index < kContextCount; index++)
                    if (gDecoders[index].kicked)
                    {
                        gDecoders[index].kicked = false;
                        Work(gContexts + index * kContextSize, gDecoders[index]);
                    }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }
}

// Tabla de contextos del decodificador: XAudio lee su dirección física del registro
// CONTEXT_ARRAY_ADDRESS (0x7FEA1800, en little-endian) y trabaja con ella directamente; sin ella
// cree que no hay decodificador XMA y descarta esas voces (música, sonidos de los menús).
void XmaInitialize()
{
    std::lock_guard lock(gLock);
    // Es la primera reserva física: sin este hueco quedaría en la dirección física 0, que para
    // XAudio significa "no hay tabla".
    memory::Allocate(memory::kPhysicalBase, memory::kPhysicalEnd, 0x10000, 0x1000);
    gContexts = memory::Allocate(memory::kPhysicalBase, memory::kPhysicalEnd, kContextSize * kContextCount, 0x1000);
    memory::Write32(kRegisterBase + kRegContextArrayAddress, __builtin_bswap32(gContexts & 0x1FFFFFFF));
    av_log_set_level(AV_LOG_FATAL); // sus avisos (marcas de tiempo, paquetes que no continúan) no hacen falta
    std::thread(DecoderThread).detach();
}

// uint32_t XMACreateContext(uint32_t* context)
PPC_FUNC(__imp__XMACreateContext)
{
    (void)base;
    std::lock_guard lock(gLock);
    for (uint32_t index = 0; index < kContextCount; index++)
    {
        if (gUsed[index])
            continue;
        gUsed[index] = true;
        const uint32_t context = gContexts + index * kContextSize;
        for (uint32_t offset = 0; offset < kContextSize; offset += 4)
            memory::Write32(context + offset, 0);
        memory::Write32(ctx.r3.u32, context);
        ctx.r3.u64 = 0;
        return;
    }
    std::fprintf(stderr, "[xma] XMACreateContext: no quedan contextos\n");
    ctx.r3.u64 = 0xC0000017; // STATUS_NO_MEMORY
}

// void XMAReleaseContext(XMA_CONTEXT_DATA* context)
PPC_FUNC(__imp__XMAReleaseContext)
{
    (void)base;
    std::lock_guard lock(gLock);
    const uint32_t index = (ctx.r3.u32 - gContexts) / kContextSize;
    if (gContexts && index < kContextCount)
    {
        gUsed[index] = false;
        gDecoders[index].enabled = false;
        Reset(gDecoders[index]);
    }
    ctx.r3.u64 = 0;
}

// uint32_t XMAInitializeContext(XMA_CONTEXT_DATA* context, XMA_CONTEXT_INIT* init)
// XMA_CONTEXT_INIT: input0 ptr, input0 packets, input1 ptr, input1 packets, read offset,
// output ptr, output blocks, work buffer, subframes, canales, frecuencia y datos de bucle.
PPC_FUNC(__imp__XMAInitializeContext)
{
    (void)base;
    const uint32_t context = ctx.r3.u32, init = ctx.r4.u32;
    std::lock_guard lock(gLock);
    if (IndexOf(context) < kContextCount)
        Reset(gDecoders[IndexOf(context)]);
    for (uint32_t offset = 0; offset < kContextSize; offset += 4)
        memory::Write32(context + offset, 0);
    auto field = [&](uint32_t index) { return memory::Read32(init + index * 4); };
    Set(context, kInput0Pointer, field(0));
    Set(context, kInput0PacketCount, field(1));
    Set(context, kInput1Pointer, field(2));
    Set(context, kInput1PacketCount, field(3));
    Set(context, kInputReadOffset, field(4));
    Set(context, kOutputPointer, field(5));
    Set(context, kOutputBlockCount, field(6));
    Set(context, kWorkPointer, field(7));
    Set(context, kSubframeDecodeCount, field(8));
    Set(context, kStereo, field(9) >= 1);
    Set(context, kSampleRate, field(10));
    Set(context, kLoopStart, field(11));
    Set(context, kLoopEnd, field(12));
    const uint32_t loop = field(13); // loop count, subframe end, subframe skip (bytes)
    Set(context, kLoopCount, loop >> 24);
    Set(context, kLoopSubframeEnd, (loop >> 16) & 0xFF);
    Set(context, kLoopSubframeSkip, (loop >> 8) & 0xFF);
    if (std::getenv("PROSTREET_TRACE"))
        std::fprintf(stderr, "[xma] init 0x%08X: entrada 0x%08X (%u paq.) / 0x%08X (%u paq.), salida 0x%08X (%u bloques), "
            "frecuencia %u, canales %u, bucle %u-%u x%u\n", context, field(0), field(1), field(2), field(3), field(5),
            field(6), field(10), field(9), field(11), field(12), loop >> 24);
    ctx.r3.u64 = 0;
}

// Arrancan y paran el decodificador de un contexto.
PPC_FUNC(__imp__XMAEnableContext)
{
    (void)base;
    {
        std::lock_guard lock(gLock);
        // Reactivar no reinicia el decodificador: la música en streaming para y reactiva su
        // contexto en cada ciclo, dejándolo como recién empezado, y hay que seguir donde iba
        // (frames a caballo entre paquetes, PCM que aún no ha cabido). Los sonidos nuevos pasan
        // por XMAInitializeContext, que sí lo reinicia.
        if (IndexOf(ctx.r3.u32) < kContextCount)
        {
            gDecoders[IndexOf(ctx.r3.u32)].enabled = true;
            gDecoders[IndexOf(ctx.r3.u32)].kicked = true;
            gDecoders[IndexOf(ctx.r3.u32)].kicks++;
        }
    }
    if (std::getenv("PROSTREET_TRACE"))
    {
        std::fprintf(stderr, "[xma] activar 0x%08X:", ctx.r3.u32);
        for (uint32_t i = 0; i < 16; i++)
            std::fprintf(stderr, " %08X", memory::Read32(ctx.r3.u32 + i * 4));
        std::fprintf(stderr, "\n");
    }
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XMADisableContext)
{
    (void)base;
    {
        std::lock_guard lock(gLock);
        if (IndexOf(ctx.r3.u32) < kContextCount)
        {
            Decoder& decoder = gDecoders[IndexOf(ctx.r3.u32)];
            if (decoder.enabled && std::getenv("PROSTREET_TRACE"))
                std::fprintf(stderr, "[xma] fin 0x%08X: %u paquetes, %u muestras, pico %.3f, %s, %u Hz, %u activaciones, "
                    "%u lecturas de salida (escritura %u, lectura %u, válida %u)\n", ctx.r3.u32, decoder.packets, decoder.samples,
                    decoder.peak, Get(ctx.r3.u32, kStereo) ? "estéreo" : "mono", decoder.codec ? decoder.codec->sample_rate : 0,
                    decoder.kicks, decoder.consumed, Get(ctx.r3.u32, kOutputWriteOffset), Get(ctx.r3.u32, kOutputReadOffset),
                    Get(ctx.r3.u32, kOutputValid));
            decoder.enabled = false;
        }
    }
    if (std::getenv("PROSTREET_TRACE"))
        std::fprintf(stderr, "[xma] desactivar 0x%08X\n", ctx.r3.u32);
    ctx.r3.u64 = 0;
}

// Búferes de entrada: (context, buffer, packetCount) y sus indicadores de validez.
PPC_FUNC(__imp__XMASetInputBuffer0)
{
    (void)base;
    std::lock_guard lock(gLock);
    if (std::getenv("PROSTREET_TRACE"))
        std::fprintf(stderr, "[xma] entrada0 0x%08X: 0x%08X %u paq.\n", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    Set(ctx.r3.u32, kInput0Pointer, ctx.r4.u32);
    Set(ctx.r3.u32, kInput0PacketCount, ctx.r5.u32);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XMASetInputBuffer1)
{
    (void)base;
    std::lock_guard lock(gLock);
    Set(ctx.r3.u32, kInput1Pointer, ctx.r4.u32);
    Set(ctx.r3.u32, kInput1PacketCount, ctx.r5.u32);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XMAIsInputBuffer0Valid)
{
    (void)base;
    std::lock_guard lock(gLock);
    ctx.r3.u64 = Get(ctx.r3.u32, kInput0Valid);
}

PPC_FUNC(__imp__XMAIsInputBuffer1Valid)
{
    (void)base;
    std::lock_guard lock(gLock);
    ctx.r3.u64 = Get(ctx.r3.u32, kInput1Valid);
}

PPC_FUNC(__imp__XMASetInputBuffer0Valid)
{
    (void)base;
    std::lock_guard lock(gLock);
    Set(ctx.r3.u32, kInput0Valid, 1);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XMASetInputBuffer1Valid)
{
    (void)base;
    std::lock_guard lock(gLock);
    Set(ctx.r3.u32, kInput1Valid, 1);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XMASetInputBufferReadOffset)
{
    (void)base;
    std::lock_guard lock(gLock);
    Set(ctx.r3.u32, kInputReadOffset, ctx.r4.u32);
    ctx.r3.u64 = 0;
}

// Búfer de salida (PCM, en bloques de 256 bytes).
PPC_FUNC(__imp__XMAGetOutputBufferWriteOffset)
{
    (void)base;
    std::lock_guard lock(gLock);
    ctx.r3.u64 = Get(ctx.r3.u32, kOutputWriteOffset);
}

PPC_FUNC(__imp__XMAGetOutputBufferReadOffset)
{
    (void)base;
    std::lock_guard lock(gLock);
    ctx.r3.u64 = Get(ctx.r3.u32, kOutputReadOffset);
}

PPC_FUNC(__imp__XMASetOutputBufferReadOffset)
{
    (void)base;
    std::lock_guard lock(gLock);
    if (IndexOf(ctx.r3.u32) < kContextCount && Get(ctx.r3.u32, kOutputReadOffset) != ctx.r4.u32)
        gDecoders[IndexOf(ctx.r3.u32)].consumed++;
    Set(ctx.r3.u32, kOutputReadOffset, ctx.r4.u32);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XMAIsOutputBufferValid)
{
    (void)base;
    std::lock_guard lock(gLock);
    ctx.r3.u64 = Get(ctx.r3.u32, kOutputValid);
}

PPC_FUNC(__imp__XMASetOutputBufferValid)
{
    (void)base;
    std::lock_guard lock(gLock);
    Set(ctx.r3.u32, kOutputValid, 1);
    ctx.r3.u64 = 0;
}
