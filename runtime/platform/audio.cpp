// Salida de sonido con un AudioUnit de CoreAudio (la salida por defecto del Mac). Ver audio.h.

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "audio.h"

namespace platform
{
    namespace
    {
        constexpr double kSampleRate = 48000.0;
        constexpr uint32_t kQueueFrames = 4;   // bloques en cola: 4 x 5,33 ms de latencia
        constexpr uint32_t kRingFrames = 16;   // capacidad de la cola

        std::mutex gLock;
        std::condition_variable gSpace;
        std::vector<float> gRing(kRingFrames * kAudioFrameSamples * 2); // estéreo entrelazado
        uint32_t gHead = 0, gCount = 0, gPosition = 0; // bloque que suena, cuántos hay, muestra dentro
        bool gRunning = false;
        AudioComponentInstance gUnit = nullptr;

        // Pide CoreAudio: se rellena con lo que haya en la cola, o silencio si no llega.
        OSStatus Render(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames,
            AudioBufferList* buffers)
        {
            auto* out = static_cast<float*>(buffers->mBuffers[0].mData);
            uint32_t consumed = 0;
            {
                std::lock_guard lock(gLock);
                for (UInt32 i = 0; i < frames; i++)
                {
                    if (gCount == 0)
                    {
                        out[i * 2] = out[i * 2 + 1] = 0.0f;
                        continue;
                    }
                    const float* frame = &gRing[size_t(gHead) * kAudioFrameSamples * 2];
                    out[i * 2] = frame[gPosition * 2];
                    out[i * 2 + 1] = frame[gPosition * 2 + 1];
                    if (++gPosition == kAudioFrameSamples)
                    {
                        gPosition = 0;
                        gHead = (gHead + 1) % kRingFrames;
                        gCount--;
                        consumed++;
                    }
                }
            }
            if (consumed)
                gSpace.notify_all();
            return noErr;
        }
    }

    bool StartAudio()
    {
        if (std::getenv("PROSTREET_NO_AUDIO"))
            return false;
        AudioComponentDescription description{};
        description.componentType = kAudioUnitType_Output;
        description.componentSubType = kAudioUnitSubType_DefaultOutput;
        description.componentManufacturer = kAudioUnitManufacturer_Apple;
        AudioComponent component = AudioComponentFindNext(nullptr, &description);
        if (!component || AudioComponentInstanceNew(component, &gUnit) != noErr)
        {
            std::fprintf(stderr, "[audio] no hay salida de sonido: el juego seguirá en silencio\n");
            return false;
        }

        AudioStreamBasicDescription format{};
        format.mSampleRate = kSampleRate;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        format.mChannelsPerFrame = 2;
        format.mBitsPerChannel = 32;
        format.mBytesPerFrame = 8;
        format.mFramesPerPacket = 1;
        format.mBytesPerPacket = 8;
        AURenderCallbackStruct callback{Render, nullptr};
        if (AudioUnitSetProperty(gUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof(format)) != noErr ||
            AudioUnitSetProperty(gUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof(callback)) != noErr ||
            AudioUnitInitialize(gUnit) != noErr || AudioOutputUnitStart(gUnit) != noErr)
        {
            std::fprintf(stderr, "[audio] no se pudo abrir la salida de sonido: el juego seguirá en silencio\n");
            return false;
        }
        gRunning = true;
        std::fprintf(stderr, "[audio] salida de sonido: 48 kHz estéreo\n");
        return true;
    }

    void WaitForAudioSpace()
    {
        std::unique_lock lock(gLock);
        gSpace.wait_for(lock, std::chrono::milliseconds(20), [] { return gCount < kQueueFrames; });
    }

    void SubmitAudioFrame(const float* planar)
    {
        if (!gRunning)
            return;
        // 5.1 (delantero izquierdo y derecho, centro, graves, trasero izquierdo y derecho) a estéreo.
        // Se suman hasta tres canales por lado: la mezcla baja un 25 % para no saturar.
        constexpr float kSide = 0.7071f, kGain = 0.75f;
        float stereo[kAudioFrameSamples * 2];
        for (uint32_t i = 0; i < kAudioFrameSamples; i++)
        {
            const float frontLeft = planar[0 * kAudioFrameSamples + i], frontRight = planar[1 * kAudioFrameSamples + i];
            const float center = planar[2 * kAudioFrameSamples + i];
            const float backLeft = planar[4 * kAudioFrameSamples + i], backRight = planar[5 * kAudioFrameSamples + i];
            stereo[i * 2] = std::clamp(kGain * (frontLeft + kSide * (center + backLeft)), -1.0f, 1.0f);
            stereo[i * 2 + 1] = std::clamp(kGain * (frontRight + kSide * (center + backRight)), -1.0f, 1.0f);
        }
        // PROSTREET_AUDIO_STATS: pico de cada segundo (para comprobar que llega sonido).
        static const bool stats = std::getenv("PROSTREET_AUDIO_STATS") != nullptr;
        if (stats)
        {
            static float peak = 0.0f, channelPeak[kAudioChannels] = {};
            static uint32_t frames = 0;
            for (float sample : stereo)
                peak = std::max(peak, std::fabs(sample));
            for (uint32_t c = 0; c < kAudioChannels; c++)
                for (uint32_t i = 0; i < kAudioFrameSamples; i++)
                    channelPeak[c] = std::max(channelPeak[c], std::fabs(planar[c * kAudioFrameSamples + i]));
            if (++frames == uint32_t(kSampleRate / kAudioFrameSamples))
            {
                std::fprintf(stderr, "[audio] pico %.3f (canales %.3f %.3f %.3f %.3f %.3f %.3f)\n", peak, channelPeak[0],
                    channelPeak[1], channelPeak[2], channelPeak[3], channelPeak[4], channelPeak[5]);
                peak = 0.0f;
                std::fill(std::begin(channelPeak), std::end(channelPeak), 0.0f);
                frames = 0;
            }
        }
        std::lock_guard lock(gLock);
        if (gCount == kRingFrames) // la salida no da abasto: se pierde el más antiguo
        {
            gHead = (gHead + 1) % kRingFrames;
            gCount--;
            gPosition = 0;
        }
        const uint32_t tail = (gHead + gCount) % kRingFrames;
        std::memcpy(&gRing[size_t(tail) * kAudioFrameSamples * 2], stereo, sizeof(stereo));
        gCount++;
    }
}
